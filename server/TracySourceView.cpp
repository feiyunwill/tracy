#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>

#include <capstone/capstone.h>

#include "../imgui/imgui.h"
#include "TracyCharUtil.hpp"
#include "TracyColor.hpp"
#include "TracyFilesystem.hpp"
#include "TracyImGui.hpp"
#include "TracyMicroArchitecture.hpp"
#include "TracyPrint.hpp"
#include "TracySort.hpp"
#include "TracySourceView.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"

#include "IconsFontAwesome5.h"

namespace tracy
{

struct MicroArchUx
{
    const char* uArch;
    const char* cpuName;
    const char* moniker;
};

static constexpr MicroArchUx s_uArchUx[] = {
    { "Conroe", "Core 2 Duo E6750", "CON" },
    { "Wolfdale", "Core 2 Duo E8400", "WOL" },
    { "Nehalem", "Core i5-750", "NHM" },
    { "Westmere", "Core i5-650", "WSM" },
    { "Sandy Bridge", "Core i7-2600", "SNB" },
    { "Ivy Bridge", "Core i5-3470", "IVB" },
    { "Haswell", "Xeon E3-1225 v3", "HSW" },
    { "Broadwell", "Core i5-5200U", "BDW" },
    { "Skylake", "Core i7-6500U", "SKL" },
    { "Skylake-X", "Core i9-7900X", "SKX" },
    { "Kaby Lake", "Core i7-7700", "KBL" },
    { "Coffee Lake", "Core i7-8700K", "CFL" },
    { "Cannon Lake", "Core i3-8121U", "CNL" },
    { "Ice Lake", "Core i5-1035G1", "ICL" },
    { "AMD Zen+", "Ryzen 5 2600", "ZEN+" },
    { "AMD Zen 2", "Ryzen 7 3700X", "ZEN2" },
};


enum { JumpSeparation = 6 };
enum { JumpArrowBase = 9 };

SourceView::SourceView( ImFont* font )
    : m_font( font )
    , m_file( nullptr )
    , m_fileStringIdx( 0 )
    , m_symAddr( 0 )
    , m_targetAddr( 0 )
    , m_data( nullptr )
    , m_dataSize( 0 )
    , m_targetLine( 0 )
    , m_selectedLine( 0 )
    , m_hoveredLine( 0 )
    , m_hoveredSource( 0 )
    , m_codeLen( 0 )
    , m_highlightAddr( 0 )
    , m_asmRelative( false )
    , m_asmBytes( false )
    , m_asmShowSourceLocation( true )
    , m_calcInlineStats( true )
    , m_showJumps( true )
    , m_cpuArch( CpuArchUnknown )
    , m_showLatency( false )
{
    SelectMicroArchitecture( "ZEN2" );

    m_microArchOpMap.reserve( OpsNum );
    for( int i=0; i<OpsNum; i++ )
    {
        m_microArchOpMap.emplace( OpsList[i], i );
    }
}

SourceView::~SourceView()
{
    delete[] m_data;
}

void SourceView::OpenSource( const char* fileName, int line, const View& view )
{
    m_targetLine = line;
    m_selectedLine = line;
    m_targetAddr = 0;
    m_baseAddr = 0;
    m_symAddr = 0;
    m_sourceFiles.clear();

    ParseSource( fileName, nullptr, view );
    assert( !m_lines.empty() );
}

void SourceView::OpenSymbol( const char* fileName, int line, uint64_t baseAddr, uint64_t symAddr, const Worker& worker, const View& view )
{
    m_targetLine = line;
    m_targetAddr = symAddr;
    m_baseAddr = baseAddr;
    m_symAddr = symAddr;
    m_sourceFiles.clear();
    m_selectedAddresses.clear();
    m_selectedAddresses.emplace( symAddr );

    ParseSource( fileName, &worker, view );
    Disassemble( baseAddr, worker );
    SelectLine( line, &worker, true, symAddr );

    if( !m_lines.empty() )
    {
        if( !m_asm.empty() )
        {
            m_displayMode = DisplayMixed;
        }
        else
        {
            m_displayMode = DisplaySource;
        }
    }
    else
    {
        assert( !m_asm.empty() );
        m_displayMode = DisplayAsm;
    }
}

void SourceView::ParseSource( const char* fileName, const Worker* worker, const View& view )
{
    if( m_file != fileName )
    {
        m_file = fileName;
        m_fileStringIdx = worker ? worker->FindStringIdx( fileName ) : 0;
        m_lines.clear();
        if( fileName )
        {
            FILE* f = fopen( view.SourceSubstitution( fileName ), "rb" );
            fseek( f, 0, SEEK_END );
            const auto sz = ftell( f );
            fseek( f, 0, SEEK_SET );
            if( sz > m_dataSize )
            {
                delete[] m_data;
                m_data = new char[sz+1];
                m_dataSize = sz;
            }
            fread( m_data, 1, sz, f );
            m_data[sz] = '\0';
            fclose( f );

            m_tokenizer.Reset();
            auto txt = m_data;
            for(;;)
            {
                auto end = txt;
                while( *end != '\n' && *end != '\r' && end - m_data < sz ) end++;
                m_lines.emplace_back( Line { txt, end, Tokenize( txt, end ) } );
                if( *end == '\n' )
                {
                    end++;
                    if( *end == '\r' ) end++;
                }
                else if( *end == '\r' )
                {
                    end++;
                    if( *end == '\n' ) end++;
                }
                if( *end == '\0' ) break;
                txt = end;
            }
        }
    }
}

bool SourceView::Disassemble( uint64_t symAddr, const Worker& worker )
{
    m_asm.clear();
    m_jumpTable.clear();
    m_jumpOut.clear();
    m_maxJumpLevel = 0;
    if( symAddr == 0 ) return false;
    m_cpuArch = worker.GetCpuArch();
    if( m_cpuArch == CpuArchUnknown ) return false;
    uint32_t len;
    auto code = worker.GetSymbolCode( symAddr, len );
    if( !code ) return false;
    m_disasmFail = -1;
    csh handle;
    cs_err rval = CS_ERR_ARCH;
    switch( m_cpuArch )
    {
    case CpuArchX86:
        rval = cs_open( CS_ARCH_X86, CS_MODE_32, &handle );
        break;
    case CpuArchX64:
        rval = cs_open( CS_ARCH_X86, CS_MODE_64, &handle );
        break;
    case CpuArchArm32:
        rval = cs_open( CS_ARCH_ARM, CS_MODE_ARM, &handle );
        break;
    case CpuArchArm64:
        rval = cs_open( CS_ARCH_ARM64, CS_MODE_ARM, &handle );
        break;
    default:
        assert( false );
        break;
    }
    if( rval != CS_ERR_OK ) return false;
    cs_option( handle, CS_OPT_DETAIL, CS_OPT_ON );
    cs_insn* insn;
    size_t cnt = cs_disasm( handle, (const uint8_t*)code, len, symAddr, 0, &insn );
    if( cnt > 0 )
    {
        if( insn[cnt-1].address - symAddr + insn[cnt-1].size < len ) m_disasmFail = insn[cnt-1].address - symAddr;
        int bytesMax = 0;
        int mLenMax = 0;
        m_asm.reserve( cnt );
        for( size_t i=0; i<cnt; i++ )
        {
            const auto& op = insn[i];
            const auto& detail = *op.detail;
            bool hasJump = false;
            for( auto j=0; j<detail.groups_count; j++ )
            {
                if( detail.groups[j] == CS_GRP_JUMP || detail.groups[j] == CS_GRP_CALL || detail.groups[j] == CS_GRP_RET )
                {
                    hasJump = true;
                    break;
                }
            }
            uint64_t jumpAddr = 0;
            if( hasJump )
            {
                switch( m_cpuArch )
                {
                case CpuArchX86:
                case CpuArchX64:
                    if( detail.x86.op_count == 1 && detail.x86.operands[0].type == X86_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.x86.operands[0].imm;
                    }
                    break;
                case CpuArchArm32:
                    if( detail.arm.op_count == 1 && detail.arm.operands[0].type == ARM_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.arm.operands[0].imm;
                    }
                    break;
                case CpuArchArm64:
                    if( detail.arm64.op_count == 1 && detail.arm64.operands[0].type == ARM64_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.arm64.operands[0].imm;
                    }
                    break;
                default:
                    assert( false );
                    break;
                }
                if( jumpAddr >= symAddr && jumpAddr < symAddr + len )
                {
                    auto fit = std::lower_bound( insn, insn+cnt, jumpAddr, []( const auto& l, const auto& r ) { return l.address < r; } );
                    if( fit != insn+cnt && fit->address == jumpAddr )
                    {
                        const auto min = std::min( jumpAddr, op.address );
                        const auto max = std::max( jumpAddr, op.address );
                        auto it = m_jumpTable.find( jumpAddr );
                        if( it == m_jumpTable.end() )
                        {
                            m_jumpTable.emplace( jumpAddr, JumpData { min, max, 0, { op.address } } );
                        }
                        else
                        {
                            if( it->second.min > min ) it->second.min = min;
                            else if( it->second.max < max ) it->second.max = max;
                            it->second.source.emplace_back( op.address );
                        }
                    }
                    else
                    {
                        jumpAddr = 0;
                    }
                }
                else
                {
                    m_jumpOut.emplace( op.address );
                }
            }
            std::vector<AsmOpParams> params;
            switch( m_cpuArch )
            {
            case CpuArchX86:
            case CpuArchX64:
                for( uint8_t i=0; i<detail.x86.op_count; i++ )
                {
                    uint8_t type = 0;
                    switch( detail.x86.operands[i].type )
                    {
                    case X86_OP_IMM:
                        type = 0;
                        break;
                    case X86_OP_REG:
                        type = 1;
                        break;
                    case X86_OP_MEM:
                        type = 2;
                        break;
                    default:
                        assert( false );
                        break;
                    }
                    params.emplace_back( AsmOpParams { type, uint16_t( detail.x86.operands[i].size * 8 ) } );
                }
                break;
            case CpuArchArm32:
                for( uint8_t i=0; i<detail.arm.op_count; i++ )
                {
                    uint8_t type = 0;
                    switch( detail.arm.operands[i].type )
                    {
                    case ARM_OP_IMM:
                        type = 0;
                        break;
                    case ARM_OP_REG:
                        type = 1;
                        break;
                    case ARM_OP_MEM:
                        type = 2;
                        break;
                    default:
                        type = 255;
                        break;
                    }
                    params.emplace_back( AsmOpParams { type, 0 } );
                }
                break;
            case CpuArchArm64:
                for( uint8_t i=0; i<detail.arm64.op_count; i++ )
                {
                    uint8_t type = 0;
                    switch( detail.arm64.operands[i].type )
                    {
                    case ARM64_OP_IMM:
                        type = 0;
                        break;
                    case ARM64_OP_REG:
                        type = 1;
                        break;
                    case ARM64_OP_MEM:
                        type = 2;
                        break;
                    default:
                        type = 255;
                        break;
                    }
                    params.emplace_back( AsmOpParams { type, 0 } );
                }
                break;
            default:
                assert( false );
                break;
            }
            LeaData leaData = LeaData::none;
            if( ( m_cpuArch == CpuArchX64 || m_cpuArch == CpuArchX86 ) && op.id == X86_INS_LEA )
            {
                assert( op.detail->x86.op_count == 2 );
                assert( op.detail->x86.operands[1].type == X86_OP_MEM );
                auto& mem = op.detail->x86.operands[1].mem;
                if( mem.base == X86_REG_INVALID )
                {
                    if( mem.index == X86_REG_INVALID )
                    {
                        leaData = LeaData::d;
                    }
                    else
                    {
                        leaData = mem.disp == 0 ? LeaData::i : LeaData::id;
                    }
                }
                else if( mem.base == X86_REG_RIP )
                {
                    leaData = mem.disp == 0 ? LeaData::r : LeaData::rd;
                }
                else
                {
                    if( mem.index == X86_REG_INVALID )
                    {
                        leaData = mem.disp == 0 ? LeaData::b : LeaData::bd;
                    }
                    else
                    {
                        leaData = mem.disp == 0 ? LeaData::bi : LeaData::bid;
                    }
                }
            }
            m_asm.emplace_back( AsmLine { op.address, jumpAddr, op.mnemonic, op.op_str, (uint8_t)op.size, leaData, std::move( params ) } );
            const auto mLen = strlen( op.mnemonic );
            if( mLen > mLenMax ) mLenMax = mLen;
            if( op.size > bytesMax ) bytesMax = op.size;

            uint32_t mLineMax = 0;
            uint32_t srcline;
            const auto srcidx = worker.GetLocationForAddress( op.address, srcline );
            if( srcline != 0 )
            {
                if( srcline > mLineMax ) mLineMax = srcline;
                const auto idx = srcidx.Idx();
                auto sit = m_sourceFiles.find( idx );
                if( sit == m_sourceFiles.end() )
                {
                    m_sourceFiles.emplace( idx, srcline );
                }
            }
            char tmp[16];
            sprintf( tmp, "%" PRIu32, mLineMax );
            m_maxLine = strlen( tmp ) + 1;
        }
        cs_free( insn, cnt );
        m_maxMnemonicLen = mLenMax + 2;
        m_maxAsmBytes = bytesMax;
        if( !m_jumpTable.empty() )
        {
            struct JumpRange
            {
                uint64_t target;
                uint64_t len;
            };
            std::vector<JumpRange> jumpRange;
            jumpRange.reserve( m_jumpTable.size() );
            for( auto& v : m_jumpTable )
            {
                pdqsort_branchless( v.second.source.begin(), v.second.source.end() );
                jumpRange.emplace_back( JumpRange { v.first, v.second.max - v.second.min } );
            }
            pdqsort_branchless( jumpRange.begin(), jumpRange.end(), []( const auto& l, const auto& r ) { return l.len < r.len; } );
            std::vector<std::vector<std::pair<uint64_t, uint64_t>>> levelRanges;
            for( auto& v : jumpRange )
            {
                auto it = m_jumpTable.find( v.target );
                assert( it != m_jumpTable.end() );
                int level = 0;
                for(;;)
                {
                    assert( levelRanges.size() >= level );
                    if( levelRanges.size() == level )
                    {
                        it->second.level = level;
                        levelRanges.push_back( { { it->second.min, it->second.max } } );
                        break;
                    }
                    else
                    {
                        bool validFit = true;
                        auto& lr = levelRanges[level];
                        for( auto& range : lr )
                        {
                            assert( !( it->second.min >= range.first && it->second.max <= range.second ) );
                            if( it->second.min <= range.second && it->second.max >= range.first )
                            {
                                validFit = false;
                                break;
                            }
                        }
                        if( validFit )
                        {
                            it->second.level = level;
                            lr.emplace_back( it->second.min, it->second.max );
                            break;
                        }
                        level++;
                    }
                }
                if( level > m_maxJumpLevel ) m_maxJumpLevel = level;
            }
        }
    }
    cs_close( &handle );
    m_codeLen = len;
    return true;
}

void SourceView::Render( const Worker& worker, const View& view )
{
    m_highlightAddr.Decay( 0 );
    m_hoveredLine.Decay( 0 );
    m_hoveredSource.Decay( 0 );

    if( m_symAddr == 0 )
    {
        if( m_file ) TextFocused( ICON_FA_FILE " File:", m_file );
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );

        RenderSimpleSourceView();
    }
    else
    {
        RenderSymbolView( worker, view );
    }
}

void SourceView::RenderSimpleSourceView()
{
    ImGui::BeginChild( "##sourceView", ImVec2( 0, 0 ), true );
    if( m_font ) ImGui::PushFont( m_font );

    auto draw = ImGui::GetWindowDrawList();
    const auto wpos = ImGui::GetWindowPos();
    const auto wh = ImGui::GetWindowHeight();
    const auto ty = ImGui::GetFontSize();
    const auto ts = ImGui::CalcTextSize( " " ).x;
    const auto lineCount = m_lines.size();
    const auto tmp = RealToString( lineCount );
    const auto maxLine = strlen( tmp );
    const auto lx = ts * maxLine + ty + round( ts*0.4f );
    draw->AddLine( wpos + ImVec2( lx, 0 ), wpos + ImVec2( lx, wh ), 0x08FFFFFF );

    if( m_targetLine != 0 )
    {
        int lineNum = 1;
        for( auto& line : m_lines )
        {
            if( m_targetLine == lineNum )
            {
                m_targetLine = 0;
                ImGui::SetScrollHereY();
            }
            RenderLine( line, lineNum++, 0, 0, 0, nullptr );
        }
    }
    else
    {
        ImGuiListClipper clipper( (int)m_lines.size() );
        while( clipper.Step() )
        {
            for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
            {
                RenderLine( m_lines[i], i+1, 0, 0, 0, nullptr );
            }
        }
    }
    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();
}

void SourceView::RenderSymbolView( const Worker& worker, const View& view )
{
    assert( m_symAddr != 0 );

    auto sym = worker.GetSymbolData( m_symAddr );
    assert( sym );
    if( sym->isInline )
    {
        auto parent = worker.GetSymbolData( m_baseAddr );
        if( parent )
        {
            TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", worker.GetString( parent->name ) );
        }
        else
        {
            char tmp[16];
            sprintf( tmp, "0x%" PRIx64, m_baseAddr );
            TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", tmp );
        }
    }
    else
    {
        TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", worker.GetString( sym->name ) );
    }

    auto inlineList = worker.GetInlineSymbolList( m_baseAddr, m_codeLen );
    if( inlineList )
    {
        SmallCheckbox( ICON_FA_SITEMAP " Function:", &m_calcInlineStats );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -1 );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        if( ImGui::BeginCombo( "##functionList", worker.GetString( sym->name ), ImGuiComboFlags_HeightLarge ) )
        {
            uint32_t totalSamples = 0;
            const auto& symStat = worker.GetSymbolStats();
            const auto symEnd = m_baseAddr + m_codeLen;
            Vector<std::pair<uint64_t, uint32_t>> symInline;
            auto baseStatIt = symStat.find( m_baseAddr );
            if( baseStatIt == symStat.end() || baseStatIt->second.excl == 0 )
            {
                symInline.push_back( std::make_pair( m_baseAddr, 0 ) );
            }
            else
            {
                symInline.push_back( std::make_pair( m_baseAddr, baseStatIt->second.excl ) );
                totalSamples += baseStatIt->second.excl;
            }
            while( *inlineList < symEnd )
            {
                if( *inlineList != m_baseAddr )
                {
                    auto statIt = symStat.find( *inlineList );
                    if( statIt == symStat.end() || statIt->second.excl == 0 )
                    {
                        symInline.push_back_non_empty( std::make_pair( *inlineList, 0 ) );
                    }
                    else
                    {
                        symInline.push_back_non_empty( std::make_pair( *inlineList, statIt->second.excl ) );
                        totalSamples += statIt->second.excl;
                    }
                }
                inlineList++;
            }
            pdqsort_branchless( symInline.begin(), symInline.end(), []( const auto& l, const auto& r ) { return l.second == r.second ? l.first < r.first : l.second > r.second; } );

            if( totalSamples == 0 )
            {
                ImGui::Columns( 2 );
                static bool widthSet = false;
                if( !widthSet )
                {
                    widthSet = true;
                    const auto w = ImGui::GetWindowWidth();
                    const auto c1 = ImGui::CalcTextSize( "0xeeeeeeeeeeeeee" ).x;
                    ImGui::SetColumnWidth( 0, w - c1 );
                    ImGui::SetColumnWidth( 1, c1 );
                }
            }
            else
            {
                ImGui::Columns( 3 );
                static bool widthSet = false;
                if( !widthSet )
                {
                    widthSet = true;
                    const auto w = ImGui::GetWindowWidth();
                    const auto c0 = ImGui::CalcTextSize( "12345678901234567890" ).x;
                    const auto c2 = ImGui::CalcTextSize( "0xeeeeeeeeeeeeee" ).x;
                    ImGui::SetColumnWidth( 0, c0 );
                    ImGui::SetColumnWidth( 1, w - c0 - c2 );
                    ImGui::SetColumnWidth( 2, c2 );
                }
            }
            for( auto& v : symInline )
            {
                if( totalSamples != 0 )
                {
                    if( v.second != 0 )
                    {
                        ImGui::TextUnformatted( TimeToString( v.second * worker.GetSamplingPeriod() ) );
                        ImGui::SameLine();
                        ImGui::TextDisabled( "(%.2f%%)", 100.f * v.second / totalSamples );
                        if( ImGui::IsItemHovered() )
                        {
                            ImGui::BeginTooltip();
                            TextFocused( "Sample count:", RealToString( v.second ) );
                            ImGui::EndTooltip();
                        }
                    }
                    ImGui::NextColumn();
                }
                auto isym = worker.GetSymbolData( v.first );
                assert( isym );
                ImGui::PushID( v.first );
                if( ImGui::Selectable( worker.GetString( isym->name ), v.first == m_symAddr, ImGuiSelectableFlags_SpanAllColumns ) )
                {
                    m_symAddr = v.first;
                }
                ImGui::PopID();
                ImGui::NextColumn();
                ImGui::TextDisabled( "0x%" PRIx64, v.first );
                ImGui::NextColumn();
            }
            ImGui::EndColumns();
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }

    TextDisabledUnformatted( "Mode:" );
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    if( !m_lines.empty() )
    {
        ImGui::RadioButton( "Source", &m_displayMode, DisplaySource );
        if( !m_asm.empty() )
        {
            ImGui::SameLine();
            ImGui::RadioButton( "Assembly", &m_displayMode, DisplayAsm );
            ImGui::SameLine();
            ImGui::RadioButton( "Combined", &m_displayMode, DisplayMixed );
        }
    }
    else
    {
        ImGui::RadioButton( "Assembly", &m_displayMode, DisplayAsm );
    }
    ImGui::PopStyleVar();

    if( !m_asm.empty() )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( ICON_FA_WEIGHT_HANGING " Code size:", MemSizeToString( m_codeLen ) );
    }

    uint32_t iptotalSrc = 0, iptotalAsm = 0;
    uint32_t ipmaxSrc = 0, ipmaxAsm = 0;
    unordered_flat_map<uint64_t, uint32_t> ipcountSrc, ipcountAsm;
    if( m_calcInlineStats )
    {
        GatherIpStats( m_symAddr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, ipmaxSrc, ipmaxAsm, worker );
    }
    else
    {
        GatherIpStats( m_baseAddr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, ipmaxSrc, ipmaxAsm, worker );
        auto iptr = worker.GetInlineSymbolList( m_baseAddr, m_codeLen );
        if( iptr )
        {
            const auto symEnd = m_baseAddr + m_codeLen;
            while( *iptr < symEnd )
            {
                GatherIpStats( *iptr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, ipmaxSrc, ipmaxAsm, worker );
                iptr++;
            }
        }
        iptotalSrc = iptotalAsm;
    }
    if( iptotalAsm > 0 )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( ICON_FA_STOPWATCH " Time:", TimeToString( iptotalAsm * worker.GetSamplingPeriod() ) );
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( ICON_FA_EYE_DROPPER " Samples:", RealToString( iptotalAsm ) );
    }

    ImGui::Separator();

    uint64_t jumpOut = 0;
    switch( m_displayMode )
    {
    case DisplaySource:
        RenderSymbolSourceView( iptotalSrc, ipcountSrc, ipcountAsm, ipmaxSrc, worker, view );
        break;
    case DisplayAsm:
        jumpOut = RenderSymbolAsmView( iptotalAsm, ipcountAsm, ipmaxAsm, worker, view );
        break;
    case DisplayMixed:
        ImGui::Columns( 2 );
        RenderSymbolSourceView( iptotalSrc, ipcountSrc, ipcountAsm, ipmaxSrc, worker, view );
        ImGui::NextColumn();
        jumpOut = RenderSymbolAsmView( iptotalAsm, ipcountAsm, ipmaxAsm, worker, view );
        ImGui::EndColumns();
        break;
    default:
        assert( false );
        break;
    }

    if( jumpOut != 0 )
    {
        auto sym = worker.GetSymbolData( jumpOut );
        if( sym )
        {
            auto line = sym->line;
            auto file = line == 0 ? nullptr : worker.GetString( sym->file );
            if( file && !SourceFileValid( file, worker.GetCaptureTime(), view ) )
            {
                file = nullptr;
                line = 0;
            }
            if( line > 0 || sym->size.Val() > 0 )
            {
                OpenSymbol( file, line, jumpOut, jumpOut, worker, view );
            }
        }
    }
}

static uint32_t GetHotnessColor( uint32_t ipSum, uint32_t maxIpCount )
{
    const auto ipPercent = float( ipSum ) / maxIpCount;
    if( ipPercent <= 0.5f )
    {
        const auto a = int( ( ipPercent * 1.5f + 0.25f ) * 255 );
        return 0x000000FF | ( a << 24 );
    }
    else if( ipPercent <= 1.f )
    {
        const auto g = int( ( ipPercent - 0.5f ) * 511 );
        return 0xFF0000FF | ( g << 8 );
    }
    else if( ipPercent <= 2.f )
    {
        const auto b = int( ( ipPercent - 1.f ) * 255 );
        return 0xFF00FFFF | ( b << 16 );
    }
    else
    {
        return 0xFFFFFFFF;
    }

}

void SourceView::RenderSymbolSourceView( uint32_t iptotal, unordered_flat_map<uint64_t, uint32_t> ipcount, unordered_flat_map<uint64_t, uint32_t> ipcountAsm, uint32_t ipmax, const Worker& worker, const View& view )
{
    if( m_sourceFiles.empty() )
    {
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
    }
    else
    {
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        if( ImGui::IsItemHovered() )
        {
            ImGui::BeginTooltip();
            TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
            ImGui::SameLine();
            TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
            ImGui::SameLine();
            TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        TextDisabledUnformatted( ICON_FA_FILE " File:" );
        ImGui::SameLine();
        const auto fileColor = GetHsvColor( m_fileStringIdx, 0 );
        SmallColorBox( fileColor );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -1 );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        if( ImGui::BeginCombo( "##fileList", m_file, ImGuiComboFlags_HeightLarge ) )
        {
            if( m_asm.empty() )
            {
                for( auto& v : m_sourceFiles )
                {
                    const auto color = GetHsvColor( v.first, 0 );
                    SmallColorBox( color );
                    ImGui::SameLine();
                    auto fstr = worker.GetString( StringIdx( v.first ) );
                    if( SourceFileValid( fstr, worker.GetCaptureTime(), view ) )
                    {
                        ImGui::PushID( v.first );
                        if( ImGui::Selectable( fstr, fstr == m_file ) )
                        {
                            ParseSource( fstr, &worker, view );
                            m_targetLine = v.second;
                            SelectLine( v.second, &worker );
                        }
                        ImGui::PopID();
                    }
                    else
                    {
                        TextDisabledUnformatted( fstr );
                    }
                }
            }
            else
            {
                uint32_t totalSamples = 0;
                unordered_flat_map<uint32_t, uint32_t> fileCounts;
                for( auto& v : m_asm )
                {
                    uint32_t srcline;
                    const auto srcidx = worker.GetLocationForAddress( v.addr, srcline );
                    if( srcline != 0 )
                    {
                        uint32_t cnt = 0;
                        auto ait = ipcountAsm.find( v.addr );
                        if( ait != ipcountAsm.end() ) cnt = ait->second;

                        auto fit = fileCounts.find( srcidx.Idx() );
                        if( fit == fileCounts.end() )
                        {
                            fileCounts.emplace( srcidx.Idx(), cnt );
                        }
                        else if( cnt != 0 )
                        {
                            fit->second += cnt;
                        }
                        totalSamples += cnt;
                    }
                }
                std::vector<std::pair<uint32_t, uint32_t>> fileCountsVec;
                fileCountsVec.reserve( fileCounts.size() );
                for( auto& v : fileCounts ) fileCountsVec.emplace_back( v.first, v.second );
                pdqsort_branchless( fileCountsVec.begin(), fileCountsVec.end(), [&worker] (const auto& l, const auto& r ) { return l.second == r.second ? strcmp( worker.GetString( l.first ), worker.GetString( r.first ) ) < 0 : l.second > r.second; } );

                if( totalSamples != 0 )
                {
                    ImGui::Columns( 2 );
                    static bool widthSet = false;
                    if( !widthSet )
                    {
                        widthSet = true;
                        const auto w = ImGui::GetWindowWidth();
                        const auto c0 = ImGui::CalcTextSize( "12345678901234567890" ).x;
                        ImGui::SetColumnWidth( 0, c0 );
                        ImGui::SetColumnWidth( 1, w - c0 );
                    }
                }
                for( auto& v : fileCountsVec )
                {
                    if( totalSamples != 0 )
                    {
                        auto fit = fileCounts.find( v.first );
                        assert( fit != fileCounts.end() );
                        if( fit->second != 0 )
                        {
                            ImGui::TextUnformatted( TimeToString( fit->second * worker.GetSamplingPeriod() ) );
                            ImGui::SameLine();
                            ImGui::TextDisabled( "(%.2f%%)", 100.f * fit->second / totalSamples );
                            if( ImGui::IsItemHovered() )
                            {
                                ImGui::BeginTooltip();
                                TextFocused( "Sample count:", RealToString( fit->second ) );
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::NextColumn();
                    }
                    const auto color = GetHsvColor( v.first, 0 );
                    SmallColorBox( color );
                    ImGui::SameLine();
                    auto fstr = worker.GetString( StringIdx( v.first ) );
                    if( SourceFileValid( fstr, worker.GetCaptureTime(), view ) )
                    {
                        ImGui::PushID( v.first );
                        if( ImGui::Selectable( fstr, fstr == m_file, ImGuiSelectableFlags_SpanAllColumns ) )
                        {
                            uint32_t line = 0;
                            for( auto& file : m_sourceFiles )
                            {
                                if( file.first == v.first )
                                {
                                    line = file.second;
                                    break;
                                }
                            }
                            ParseSource( fstr, &worker, view );
                            m_targetLine = line;
                            SelectLine( line, &worker );
                        }
                        ImGui::PopID();
                    }
                    else
                    {
                        TextDisabledUnformatted( fstr );
                    }
                    if( totalSamples != 0 ) ImGui::NextColumn();
                }
                if( totalSamples != 0 ) ImGui::EndColumns();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }

    ImGui::BeginChild( "##sourceView", ImVec2( 0, 0 ), true, ImGuiWindowFlags_NoMove );
    if( m_font ) ImGui::PushFont( m_font );

    auto draw = ImGui::GetWindowDrawList();
    const auto wpos = ImGui::GetWindowPos();
    const auto wh = ImGui::GetWindowHeight();
    const auto ty = ImGui::GetFontSize();
    const auto ts = ImGui::CalcTextSize( " " ).x;
    const auto lineCount = m_lines.size();
    const auto tmp = RealToString( lineCount );
    const auto maxLine = strlen( tmp );
    auto lx = ts * maxLine + ty + round( ts*0.4f );
    if( iptotal != 0 ) lx += ts * 7 + ty;
    if( !m_asm.empty() )
    {
        const auto tmp = RealToString( m_asm.size() );
        const auto maxAsm = strlen( tmp ) + 1;
        lx += ts * maxAsm + ty;
    }
    draw->AddLine( wpos + ImVec2( lx, 0 ), wpos + ImVec2( lx, wh ), 0x08FFFFFF );

    m_selectedAddressesHover.clear();
    if( m_targetLine != 0 )
    {
        int lineNum = 1;
        for( auto& line : m_lines )
        {
            if( m_targetLine == lineNum )
            {
                m_targetLine = 0;
                ImGui::SetScrollHereY();
            }
            RenderLine( line, lineNum++, 0, iptotal, ipmax, &worker );
        }
    }
    else
    {
        ImGuiListClipper clipper( (int)m_lines.size() );
        while( clipper.Step() )
        {
            if( iptotal == 0 )
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    RenderLine( m_lines[i], i+1, 0, 0, 0, &worker );
                }
            }
            else
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    auto it = ipcount.find( i+1 );
                    const auto ipcnt = it == ipcount.end() ? 0 : it->second;
                    RenderLine( m_lines[i], i+1, ipcnt, iptotal, ipmax, &worker );
                }
            }
        }
    }

    auto win = ImGui::GetCurrentWindow();
    if( win->ScrollbarY )
    {
        auto draw = ImGui::GetWindowDrawList();
        auto rect = ImGui::GetWindowScrollbarRect( win, ImGuiAxis_Y );
        ImGui::PushClipRect( rect.Min, rect.Max, false );
        if( m_selectedLine != 0 )
        {
            const auto ly = round( rect.Min.y + ( m_selectedLine - 0.5f ) / m_lines.size() * rect.GetHeight() );
            draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x8899994C, 3 );
        }
        if( m_fileStringIdx == m_hoveredSource && m_hoveredLine != 0 )
        {
            const auto ly = round( rect.Min.y + ( m_hoveredLine - 0.5f ) / m_lines.size() * rect.GetHeight() );
            draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x88888888, 3 );
        }

        std::vector<std::pair<uint64_t, uint32_t>> ipData;
        ipData.reserve( ipcount.size() );
        for( auto& v : ipcount ) ipData.emplace_back( v.first, v.second );
        for( uint32_t lineNum = 1; lineNum <= m_lines.size(); lineNum++ )
        {
            if( ipcount.find( lineNum ) == ipcount.end() )
            {
                auto addresses = worker.GetAddressesForLocation( m_fileStringIdx, lineNum );
                if( addresses )
                {
                    for( auto& addr : *addresses )
                    {
                        if( addr >= m_baseAddr && addr < m_baseAddr + m_codeLen )
                        {
                            ipData.emplace_back( lineNum, 0 );
                            break;
                        }
                    }
                }
            }
        }
        pdqsort_branchless( ipData.begin(), ipData.end(), []( const auto& l, const auto& r ) { return l.first < r.first; } );

        const auto step = uint32_t( m_lines.size() * 2 / rect.GetHeight() );
        const auto x14 = round( rect.Min.x + rect.GetWidth() * 0.4f );
        const auto x34 = round( rect.Min.x + rect.GetWidth() * 0.6f );

        auto it = ipData.begin();
        while( it != ipData.end() )
        {
            const auto firstLine = it->first;
            uint32_t ipSum = 0;
            while( it != ipData.end() && it->first <= firstLine + step )
            {
                ipSum += it->second;
                ++it;
            }
            const auto ly = round( rect.Min.y + float( firstLine ) / m_lines.size() * rect.GetHeight() );
            const uint32_t color = ipSum == 0 ? 0x22FFFFFF : GetHotnessColor( ipSum, ipmax );
            draw->AddRectFilled( ImVec2( x14, ly ), ImVec2( x34, ly+3 ), color );
        }

        ImGui::PopClipRect();
    }

    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();
}

static int PrintHexBytes( char* buf, const uint8_t* bytes, size_t len )
{
    static constexpr char HexPrint[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    const auto start = buf;
    for( size_t i=0; i<len; i++ )
    {
        const auto byte = bytes[i];
        *buf++ = HexPrint[byte >> 4];
        *buf++ = HexPrint[byte & 0xF];
        *buf++ = ' ';
    }
    *--buf = '\0';
    return buf - start;
}

uint64_t SourceView::RenderSymbolAsmView( uint32_t iptotal, unordered_flat_map<uint64_t, uint32_t> ipcount, uint32_t ipmax, const Worker& worker, const View& view )
{
    if( m_disasmFail >= 0 )
    {
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        if( ImGui::IsItemHovered() )
        {
            const bool clicked = ImGui::IsItemClicked();
            ImGui::BeginTooltip();
            TextColoredUnformatted( ImVec4( 1, 0, 0, 1 ), "Disassembly failure." );
            ImGui::TextUnformatted( "Some instructions weren't properly decoded. Possible reasons:" );
            ImGui::TextUnformatted( " 1. Old version of capstone library doesn't support some instructions." );
            ImGui::TextUnformatted( " 2. Trying to decode data part of the symbol (e.g. jump arrays, etc.)" );
            TextFocused( "Code size:", RealToString( m_codeLen ) );
            TextFocused( "Disassembled bytes:", RealToString( m_disasmFail ) );
            char tmp[64];
            auto bytesLeft = std::min( 16u, m_codeLen - m_disasmFail );
            auto code = worker.GetSymbolCode( m_symAddr, m_codeLen );
            assert( code );
            PrintHexBytes( tmp, (const uint8_t*)code, bytesLeft );
            TextFocused( "Failure bytes:", tmp );
            TextDisabledUnformatted( "Click to copy to clipboard." );
            ImGui::EndTooltip();
            if( clicked ) ImGui::SetClipboardText( tmp );
        }
        ImGui::SameLine();
    }
    SmallCheckbox( ICON_FA_SEARCH_LOCATION " Relative locations", &m_asmRelative );
    if( !m_sourceFiles.empty() )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        SmallCheckbox( ICON_FA_FILE_IMPORT " Source locations", &m_asmShowSourceLocation );
    }
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    SmallCheckbox( ICON_FA_COGS " Machine code", &m_asmBytes );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    SmallCheckbox( ICON_FA_SHARE " Jumps", &m_showJumps );

    if( m_cpuArch == CpuArchX64 || m_cpuArch == CpuArchX86 )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        float mw = 0;
        for( auto& v : s_uArchUx )
        {
            const auto w = ImGui::CalcTextSize( v.uArch ).x;
            if( w > mw ) mw = w;
        }
        ImGui::TextUnformatted( ICON_FA_MICROCHIP " \xce\xbc""arch:" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( mw + ImGui::GetFontSize() );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        if( ImGui::BeginCombo( "##uarch", s_uArchUx[m_selMicroArch].uArch, ImGuiComboFlags_HeightLarge ) )
        {
            int idx = 0;
            for( auto& v : s_uArchUx )
            {
                if( ImGui::Selectable( v.uArch, idx == m_selMicroArch ) ) SelectMicroArchitecture( v.moniker );
                ImGui::SameLine();
                TextDisabledUnformatted( v.cpuName );
                idx++;
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        SmallCheckbox( ICON_FA_TRUCK_LOADING " Latency", &m_showLatency );
    }

    ImGui::BeginChild( "##asmView", ImVec2( 0, 0 ), true, ImGuiWindowFlags_NoMove );
    if( m_font ) ImGui::PushFont( m_font );

    int maxAddrLen;
    {
        char tmp[32];
        sprintf( tmp, "%" PRIx64, m_baseAddr + m_codeLen );
        maxAddrLen = strlen( tmp );
    }

    uint64_t selJumpStart = 0;
    uint64_t selJumpEnd;
    uint64_t selJumpTarget;
    uint64_t jumpOut = 0;

    if( m_targetAddr != 0 )
    {
        for( auto& line : m_asm )
        {
            if( m_targetAddr == line.addr )
            {
                m_targetAddr = 0;
                ImGui::SetScrollHereY();
            }
            RenderAsmLine( line, 0, iptotal, ipmax, worker, jumpOut, maxAddrLen, view );
        }
    }
    else
    {
        const auto th = (int)ImGui::GetTextLineHeightWithSpacing();
        ImGuiListClipper clipper( (int)m_asm.size(), th );
        while( clipper.Step() )
        {
            assert( clipper.StepNo == 3 );
            const auto wpos = ImGui::GetCursorScreenPos();
            static std::vector<uint64_t> insList;
            insList.clear();
            if( iptotal == 0 )
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    RenderAsmLine( m_asm[i], 0, 0, 0, worker, jumpOut, maxAddrLen, view );
                    insList.emplace_back( m_asm[i].addr );
                }
            }
            else
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    auto& line = m_asm[i];
                    auto it = ipcount.find( line.addr );
                    const auto ipcnt = it == ipcount.end() ? 0 : it->second;
                    RenderAsmLine( line, ipcnt, iptotal, ipmax, worker, jumpOut, maxAddrLen, view );
                    insList.emplace_back( line.addr );
                }
            }
            if( m_showJumps && !m_jumpTable.empty() )
            {
                auto draw = ImGui::GetWindowDrawList();
                const auto ts = ImGui::CalcTextSize( " " );
                const auto th2 = floor( ts.y / 2 );
                const auto th4 = floor( ts.y / 4 );
                const auto xoff = ( iptotal == 0 ? 0 : ( 7 * ts.x + ts.y ) ) + (3+maxAddrLen) * ts.x + ( ( m_asmShowSourceLocation && !m_sourceFiles.empty() ) ? 36 * ts.x : 0 ) + ( m_asmBytes ? m_maxAsmBytes*3 * ts.x : 0 );
                const auto minAddr = m_asm[clipper.DisplayStart].addr;
                const auto maxAddr = m_asm[clipper.DisplayEnd-1].addr;
                const auto mjl = m_maxJumpLevel;
                const auto JumpArrow = JumpArrowBase * ts.y / 15;

                int i = -1;
                for( auto& v : m_jumpTable )
                {
                    i++;
                    if( v.second.min > maxAddr || v.second.max < minAddr ) continue;
                    const auto col = GetHsvColor( i, 0 );

                    auto it0 = std::lower_bound( insList.begin(), insList.end(), v.second.min );
                    auto it1 = std::lower_bound( insList.begin(), insList.end(), v.second.max );
                    const auto y0 = ( it0 == insList.end() || *it0 != v.second.min ) ? -th : ( it0 - insList.begin() ) * th;
                    const auto y1 = it1 == insList.end() ? ( insList.size() + 1 ) * th  : ( it1 - insList.begin() ) * th;

                    float thickness = 1;
                    if( ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ) - JumpSeparation / 2, y0 + th2 ), wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ) + JumpSeparation / 2, y1 + th2 ) ) )
                    {
                        thickness = 2;
                        if( m_font ) ImGui::PopFont();
                        ImGui::BeginTooltip();
                        char tmp[32];
                        sprintf( tmp, "+%" PRIu64, v.first - m_baseAddr );
                        TextFocused( "Jump target:", tmp );
                        ImGui::SameLine();
                        sprintf( tmp, "(0x%" PRIx64 ")", v.first );
                        TextDisabledUnformatted( tmp );
                        uint32_t srcline;
                        const auto srcidx = worker.GetLocationForAddress( v.first, srcline );
                        if( srcline != 0 )
                        {
                            const auto fileName = worker.GetString( srcidx );
                            const auto fileColor = GetHsvColor( srcidx.Idx(), 0 );
                            TextDisabledUnformatted( "Target location:" );
                            ImGui::SameLine();
                            SmallColorBox( fileColor );
                            ImGui::SameLine();
                            ImGui::Text( "%s:%i", fileName, srcline );
                        }
                        TextFocused( "Jump range:", MemSizeToString( v.second.max - v.second.min ) );
                        TextFocused( "Jump sources:", RealToString( v.second.source.size() ) );
                        ImGui::EndTooltip();
                        if( m_font ) ImGui::PushFont( m_font );
                        if( ImGui::IsMouseClicked( 0 ) )
                        {
                            m_targetAddr = v.first;
                            m_selectedAddresses.clear();
                            m_selectedAddresses.emplace( v.first );
                        }
                        selJumpStart = v.second.min;
                        selJumpEnd = v.second.max;
                        selJumpTarget = v.first;
                    }

                    draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y0 + th2 ), wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y1 + th2 ), col, thickness );

                    if( v.first >= minAddr && v.first <= maxAddr )
                    {
                        auto iit = std::lower_bound( insList.begin(), insList.end(), v.first );
                        assert( iit != insList.end() );
                        const auto y = ( iit - insList.begin() ) * th;
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow + 1, y + th2 ), col, thickness );
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow - th4, y + th2 - th4 ), col, thickness );
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow - th4, y + th2 + th4 ), col, thickness );
                    }
                    for( auto& s : v.second.source )
                    {
                        if( s >= minAddr && s <= maxAddr )
                        {
                            auto iit = std::lower_bound( insList.begin(), insList.end(), s );
                            assert( iit != insList.end() );
                            const auto y = ( iit - insList.begin() ) * th;
                            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), col, thickness );
                        }
                    }
                }
            }
        }
    }

    auto win = ImGui::GetCurrentWindow();
    if( win->ScrollbarY )
    {
        auto draw = ImGui::GetWindowDrawList();
        auto rect = ImGui::GetWindowScrollbarRect( win, ImGuiAxis_Y );
        ImGui::PushClipRect( rect.Min, rect.Max, false );
        std::vector<uint32_t> lineOff;
        lineOff.reserve( std::max( m_selectedAddresses.size(), m_selectedAddressesHover.size() ) );
        if( !m_selectedAddresses.empty() )
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( m_selectedAddresses.find( m_asm[i].addr ) != m_selectedAddresses.end() )
                {
                    lineOff.push_back( uint32_t( i ) );
                }
            }
            float lastLine = 0;
            for( auto& v : lineOff )
            {
                const auto ly = round( rect.Min.y + ( v - 0.5f ) / m_asm.size() * rect.GetHeight() );
                if( ly > lastLine )
                {
                    lastLine = ly;
                    draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x8899994C, 1 );
                }
            }
        }
        if( !m_selectedAddressesHover.empty() )
        {
            lineOff.clear();
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( m_selectedAddressesHover.find( m_asm[i].addr ) != m_selectedAddressesHover.end() )
                {
                    lineOff.push_back( uint32_t( i ) );
                }
            }
            float lastLine = 0;
            for( auto& v : lineOff )
            {
                const auto ly = round( rect.Min.y + ( v - 0.5f ) / m_asm.size() * rect.GetHeight() );
                if( ly > lastLine )
                {
                    lastLine = ly;
                    draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x88888888, 1 );
                }
            }
        }

        uint32_t selJumpLineStart, selJumpLineEnd, selJumpLineTarget;
        std::vector<std::pair<uint64_t, uint32_t>> ipData;
        ipData.reserve( ipcount.size() );
        if( selJumpStart == 0 )
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                auto it = ipcount.find( m_asm[i].addr );
                if( it == ipcount.end() ) continue;
                ipData.emplace_back( i, it->second );
            }
        }
        else
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( selJumpStart == m_asm[i].addr ) selJumpLineStart = i;
                if( selJumpEnd == m_asm[i].addr ) selJumpLineEnd = i;
                if( selJumpTarget == m_asm[i].addr ) selJumpLineTarget = i;

                auto it = ipcount.find( m_asm[i].addr );
                if( it == ipcount.end() ) continue;
                ipData.emplace_back( i, it->second );
            }
        }
        pdqsort_branchless( ipData.begin(), ipData.end(), []( const auto& l, const auto& r ) { return l.first < r.first; } );

        const auto step = uint32_t( m_asm.size() * 2 / rect.GetHeight() );
        const auto x40 = round( rect.Min.x + rect.GetWidth() * 0.4f );
        const auto x60 = round( rect.Min.x + rect.GetWidth() * 0.6f );

        auto it = ipData.begin();
        while( it != ipData.end() )
        {
            const auto firstLine = it->first;
            uint32_t ipSum = 0;
            while( it != ipData.end() && it->first <= firstLine + step )
            {
                ipSum += it->second;
                ++it;
            }
            const auto ly = round( rect.Min.y + float( firstLine ) / m_asm.size() * rect.GetHeight() );
            const uint32_t color = GetHotnessColor( ipSum, ipmax );
            draw->AddRectFilled( ImVec2( x40, ly ), ImVec2( x60, ly+3 ), color );
        }

        if( selJumpStart != 0 )
        {
            const auto yStart = rect.Min.y + float( selJumpLineStart ) / m_asm.size() * rect.GetHeight();
            const auto yEnd = rect.Min.y + float( selJumpLineEnd ) / m_asm.size() * rect.GetHeight();
            const auto yTarget = rect.Min.y + float( selJumpLineTarget ) / m_asm.size() * rect.GetHeight();
            const auto x50 = round( rect.Min.x + rect.GetWidth() * 0.5f ) - 1;
            const auto x25 = round( rect.Min.x + rect.GetWidth() * 0.25f );
            const auto x75 = round( rect.Min.x + rect.GetWidth() * 0.75f );
            draw->AddLine( ImVec2( x50, yStart ), ImVec2( x50, yEnd ), 0xFF00FF00 );
            draw->AddLine( ImVec2( x25, yTarget ), ImVec2( x75, yTarget ), 0xFF00FF00 );
        }
    }

    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();

    return jumpOut;
}

static bool PrintPercentage( float val )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto wpos = ImGui::GetCursorScreenPos();
    const auto stw = ImGui::CalcTextSize( " " ).x;
    const auto htw = stw / 2;
    const auto tw = stw * 8;

    char tmp[16];
    auto end = PrintFloat( tmp, tmp+16, val, 2 );
    memcpy( end, "%", 2 );
    end++;
    const auto sz = end - tmp;
    char buf[16];
    memset( buf, ' ', 7-sz );
    memcpy( buf + 7 - sz, tmp, sz+1 );

    draw->AddRectFilled( wpos, wpos + ImVec2( val * tw / 100, ty+1 ), 0xFF444444 );
    DrawTextContrast( draw, wpos + ImVec2( htw, 0 ), 0xFFFFFFFF, buf );

    ImGui::ItemSize( ImVec2( stw * 7, ty ), 0 );
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( wpos, wpos + ImVec2( stw * 7, ty ) );
}

static const ImVec4 SyntaxColors[] = {
    { 0.7f,  0.7f,  0.7f,  1 },    // default
    { 0.45f, 0.68f, 0.32f, 1 },    // comment
    { 0.72f, 0.37f, 0.12f, 1 },    // preprocessor
    { 0.64f, 0.64f, 1,     1 },    // string
    { 0.64f, 0.82f, 1,     1 },    // char literal
    { 1,     0.91f, 0.53f, 1 },    // keyword
    { 0.81f, 0.6f,  0.91f, 1 },    // number
    { 0.9f,  0.9f,  0.9f,  1 },    // punctuation
    { 0.78f, 0.46f, 0.75f, 1 },    // type
    { 0.21f, 0.69f, 0.89f, 1 },    // special
};

void SourceView::RenderLine( const Line& line, int lineNum, uint32_t ipcnt, uint32_t iptotal, uint32_t ipmax, const Worker* worker )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ImGui::GetWindowWidth();
    const auto wpos = ImGui::GetCursorScreenPos();
    if( m_fileStringIdx == m_hoveredSource && lineNum == m_hoveredLine )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x22FFFFFF );
    }
    else if( lineNum == m_selectedLine )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF333322 );
    }

    if( iptotal != 0 )
    {
        if( ipcnt == 0 )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            ImGui::ItemSize( ImVec2( 7 * ts.x, ts.y ) );
        }
        else
        {
            if( PrintPercentage( 100.f * ipcnt / iptotal ) )
            {
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                if( worker ) TextFocused( "Time:", TimeToString( ipcnt * worker->GetSamplingPeriod() ) );
                TextFocused( "Sample count:", RealToString( ipcnt ) );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
            }
            draw->AddLine( wpos + ImVec2( 0, 1 ), wpos + ImVec2( 0, ty-2 ), GetHotnessColor( ipcnt, ipmax ) );
        }
        ImGui::SameLine( 0, ty );
    }

    const auto lineCount = m_lines.size();
    const auto tmp = RealToString( lineCount );
    const auto maxLine = strlen( tmp );
    const auto lineString = RealToString( lineNum );
    const auto linesz = strlen( lineString );
    char buf[16];
    memset( buf, ' ', maxLine - linesz );
    memcpy( buf + maxLine - linesz, lineString, linesz+1 );
    TextDisabledUnformatted( buf );
    ImGui::SameLine( 0, ty );

    uint32_t match = 0;
    if( !m_asm.empty() )
    {
        assert( worker );
        const auto stw = ImGui::CalcTextSize( " " ).x;
        auto addresses = worker->GetAddressesForLocation( m_fileStringIdx, lineNum );
        if( addresses )
        {
            for( auto& addr : *addresses )
            {
                match += ( addr >= m_baseAddr && addr < m_baseAddr + m_codeLen );
            }
        }
        const auto tmp = RealToString( m_asm.size() );
        const auto maxAsm = strlen( tmp ) + 1;
        if( match > 0 )
        {
            const auto asmString = RealToString( match );
            sprintf( buf, "@%s", asmString );
            const auto asmsz = strlen( buf );
            TextDisabledUnformatted( buf );
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * ( maxAsm - asmsz ), ty ), 0 );
        }
        else
        {
            ImGui::ItemSize( ImVec2( stw * maxAsm, ty ), 0 );
        }
    }

    ImGui::SameLine( 0, ty );
    auto ptr = line.begin;
    auto it = line.tokens.begin();
    while( ptr < line.end )
    {
        if( it == line.tokens.end() )
        {
            ImGui::TextUnformatted( ptr, line.end );
            ImGui::SameLine( 0, 0 );
            break;
        }
        if( ptr < it->begin )
        {
            ImGui::TextUnformatted( ptr, it->begin );
            ImGui::SameLine( 0, 0 );
        }
        TextColoredUnformatted( SyntaxColors[(int)it->color], it->begin, it->end );
        ImGui::SameLine( 0, 0 );
        ptr = it->end;
        ++it;
    }
    ImGui::ItemSize( ImVec2( 0, 0 ), 0 );

    if( match > 0 && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( wpos, wpos + ImVec2( w, ty+1 ) ) )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x11FFFFFF );
        if( ImGui::IsMouseClicked( 0 ) || ImGui::IsMouseClicked( 1 ) )
        {
            m_displayMode = DisplayMixed;
            SelectLine( lineNum, worker, ImGui::IsMouseClicked( 1 ) );
        }
        else
        {
            SelectAsmLinesHover( m_fileStringIdx, lineNum, *worker );
        }
    }

    draw->AddLine( wpos + ImVec2( 0, ty+2 ), wpos + ImVec2( w, ty+2 ), 0x08FFFFFF );
}

void SourceView::RenderAsmLine( const AsmLine& line, uint32_t ipcnt, uint32_t iptotal, uint32_t ipmax, const Worker& worker, uint64_t& jumpOut, int maxAddrLen, const View& view )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ImGui::GetWindowWidth();
    const auto wpos = ImGui::GetCursorScreenPos();
    if( m_selectedAddressesHover.find( line.addr ) != m_selectedAddressesHover.end() )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x22FFFFFF );
    }
    else if( m_selectedAddresses.find( line.addr ) != m_selectedAddresses.end() )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF333322 );
    }
    if( line.addr == m_highlightAddr )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF222233 );
    }

    if( iptotal != 0 )
    {
        if( ipcnt == 0 )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            ImGui::ItemSize( ImVec2( 7 * ts.x, ts.y ) );
        }
        else
        {
            if( PrintPercentage( 100.f * ipcnt / iptotal ) )
            {
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                TextFocused( "Time:", TimeToString( ipcnt * worker.GetSamplingPeriod() ) );
                TextFocused( "Sample count:", RealToString( ipcnt ) );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
            }
            draw->AddLine( wpos + ImVec2( 0, 1 ), wpos + ImVec2( 0, ty-2 ), GetHotnessColor( ipcnt, ipmax ) );

        }
        ImGui::SameLine( 0, ty );
    }

    char buf[256];
    if( m_asmRelative )
    {
        sprintf( buf, "+%" PRIu64, line.addr - m_baseAddr );
    }
    else
    {
        sprintf( buf, "%" PRIx64, line.addr );
    }
    const auto asz = strlen( buf );
    memset( buf+asz, ' ', maxAddrLen-asz );
    buf[maxAddrLen] = '\0';
    TextDisabledUnformatted( buf );

    const auto stw = ImGui::CalcTextSize( " " ).x;
    bool lineHovered = false;
    if( m_asmShowSourceLocation && !m_sourceFiles.empty() )
    {
        ImGui::SameLine();
        uint32_t srcline;
        const auto srcidx = worker.GetLocationForAddress( line.addr, srcline );
        if( srcline != 0 )
        {
            const auto fileName = worker.GetString( srcidx );
            const auto fileColor = GetHsvColor( srcidx.Idx(), 0 );
            SmallColorBox( fileColor );
            ImGui::SameLine();
            const auto lineString = RealToString( srcline );
            const auto linesz = strlen( lineString );
            char buf[32];
            const auto fnsz = strlen( fileName );
            if( fnsz < 30 - m_maxLine )
            {
                sprintf( buf, "%s:%i", fileName, srcline );
            }
            else
            {
                sprintf( buf, "...%s:%i", fileName+fnsz-(30-3-1-m_maxLine), srcline );
            }
            const auto bufsz = strlen( buf );
            TextDisabledUnformatted( buf );
            if( ImGui::IsItemHovered() )
            {
                lineHovered = true;
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                ImGui::Text( "%s:%i", fileName, srcline );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
                if( ImGui::IsItemClicked( 0 ) || ImGui::IsItemClicked( 1 ) )
                {
                    if( m_file == fileName )
                    {
                        if( ImGui::IsMouseClicked( 1 ) ) m_targetLine = srcline;
                        SelectLine( srcline, &worker, false );
                        m_displayMode = DisplayMixed;
                    }
                    else if( SourceFileValid( fileName, worker.GetCaptureTime(), view ) )
                    {
                        ParseSource( fileName, &worker, view );
                        m_targetLine = srcline;
                        SelectLine( srcline, &worker, false );
                        m_displayMode = DisplayMixed;
                    }
                    else
                    {
                        SelectAsmLines( srcidx.Idx(), srcline, worker, false );
                    }
                }
                else
                {
                    m_hoveredLine = srcline;
                    m_hoveredSource = srcidx.Idx();
                }
            }
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * ( 32 - bufsz ), ty ), 0 );
        }
        else
        {
            SmallColorBox( 0 );
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * 32, ty ), 0 );
        }
    }
    if( m_asmBytes )
    {
        auto code = (const uint8_t*)worker.GetSymbolCode( m_baseAddr, m_codeLen );
        assert( code );
        char tmp[64];
        const auto len = PrintHexBytes( tmp, code + line.addr - m_baseAddr, line.len );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 0.5, 0.5, 1, 1 ), tmp );
        ImGui::SameLine( 0, 0 );
        ImGui::ItemSize( ImVec2( stw * ( m_maxAsmBytes*3 - len ), ty ), 0 );
    }
    if( m_showJumps )
    {
        const auto JumpArrow = JumpArrowBase * ty / 15;
        ImGui::SameLine( 0, 2*ty + JumpArrow + m_maxJumpLevel * JumpSeparation );
        auto jit = m_jumpOut.find( line.addr );
        if( jit != m_jumpOut.end() )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            const auto th2 = floor( ts.y / 2 );
            const auto th4 = floor( ts.y / 4 );
            const auto& mjl = m_maxJumpLevel;
            const auto col = GetHsvColor( line.jumpAddr, 6 );
            const auto xoff = ( iptotal == 0 ? 0 : ( 7 * ts.x + ts.y ) ) + (3+maxAddrLen) * ts.x + ( ( m_asmShowSourceLocation && !m_sourceFiles.empty() ) ? 36 * ts.x : 0 ) + ( m_asmBytes ? m_maxAsmBytes*3 * ts.x : 0 );

            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + JumpArrow / 2, th2 ), col );
            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + th4, th2 - th4 ), col );
            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + th4, th2 + th4 ), col );
        }
    }
    else
    {
        ImGui::SameLine( 0, ty );
    }

    const AsmVar* asmVar = nullptr;
    if( ( m_cpuArch == CpuArchX64 || m_cpuArch == CpuArchX86 ) )
    {
        auto uarch = MicroArchitectureData[m_idxMicroArch];
        char tmp[32];
        for( size_t i=0; i<line.mnemonic.size(); i++ )
        {
            auto c = line.mnemonic[i];
            if( c >= 'a' && c <= 'z' ) c = c - 'a' + 'A';
            tmp[i] = c;
        }
        tmp[line.mnemonic.size()] = '\0';
        const char* mnemonic = tmp;
        if( strcmp( mnemonic, "LEA" ) == 0 )
        {
            static constexpr const char* LeaTable[] = { "LEA", "LEA_B", "LEA_BD", "LEA_BI", "LEA_BID", "LEA_D", "LEA_I", "LEA_ID", "LEA_R", "LEA_RD" };
            mnemonic = LeaTable[(int)line.leaData];
        }
        auto it = m_microArchOpMap.find( mnemonic );
        if( it != m_microArchOpMap.end() )
        {
            const auto opid = it->second;
            auto oit = std::lower_bound( uarch->ops, uarch->ops + uarch->numOps, opid, []( const auto& l, const auto& r ) { return l->id < r; } );
            if( oit != uarch->ops + uarch->numOps && (*oit)->id == opid )
            {
                const auto& op = *oit;
                std::vector<std::pair<int, int>> res;
                res.reserve( op->numVariants );
                for( int i=0; i<op->numVariants; i++ )
                {
                    const auto& var = *op->variant[i];
                    if( var.descNum == line.params.size() )
                    {
                        int penalty = 0;
                        bool match = true;
                        for( int j=0; j<var.descNum; j++ )
                        {
                            if( var.desc[j].type != line.params[j].type )
                            {
                                match = false;
                                break;
                            }
                            if( var.desc[j].width != line.params[j].width ) penalty++;
                        }
                        if( match )
                        {
                            res.emplace_back( i, penalty );
                        }
                    }
                }
                if( !res.empty() )
                {
                    pdqsort_branchless( res.begin(), res.end(), []( const auto& l, const auto& r ) { return l.second < r.second; } );
                    asmVar = op->variant[res[0].first];
                }
            }
        }
    }

    if( m_showLatency && asmVar && asmVar->minlat >= 0 )
    {
        const auto pos = ImVec2( (int)ImGui::GetCursorScreenPos().x, (int)ImGui::GetCursorScreenPos().y );
        const auto ty = ImGui::GetFontSize();

        if( asmVar->minlat == 0 )
        {
            draw->AddLine( pos + ImVec2( 0, -1 ), pos + ImVec2( 0, ty ), 0x660000FF );
        }
        else
        {
            draw->AddRectFilled( pos, pos + ImVec2( ty * asmVar->minlat + 1, ty + 1 ), 0x660000FF );
        }
        if( asmVar->minlat != asmVar->maxlat )
        {
            draw->AddRectFilled( pos + ImVec2( ty * asmVar->minlat + 1, 0 ), pos + ImVec2( ty * asmVar->maxlat + 1, ty + 1 ), 0x5500FFFF );
        }
    }

    const auto msz = line.mnemonic.size();
    memcpy( buf, line.mnemonic.c_str(), msz );
    memset( buf+msz, ' ', m_maxMnemonicLen-msz );
    memcpy( buf+m_maxMnemonicLen, line.operands.c_str(), line.operands.size() + 1 );
    ImGui::TextUnformatted( buf );

    if( asmVar && ImGui::IsItemHovered() )
    {
        const auto& var = *asmVar;
        if( m_font ) ImGui::PopFont();
        ImGui::BeginTooltip();
        TextFocused( "Throughput:", RealToString( var.tp ) );
        ImGui::SameLine();
        TextDisabledUnformatted( "(cycles per instruction, lower is better)" );
        if( var.maxlat >= 0 )
        {
            TextDisabledUnformatted( "Latency:" );
            ImGui::SameLine();
            if( var.minlat == var.maxlat && var.minbound == var.maxbound )
            {
                if( var.minbound )
                {
                    ImGui::Text( "\xe2\x89\xa4%s", RealToString( var.minlat ) );
                }
                else
                {
                    ImGui::TextUnformatted( RealToString( var.minlat ) );
                }
            }
            else
            {
                if( var.minbound )
                {
                    ImGui::Text( "[\xe2\x89\xa4%s", RealToString( var.minlat ) );
                }
                else
                {
                    ImGui::Text( "[%s", RealToString( var.minlat ) );
                }
                ImGui::SameLine( 0, 0 );
                if( var.maxbound )
                {
                    ImGui::Text( " \xE2\x80\x93 \xe2\x89\xa4%s]", RealToString( var.maxlat ) );
                }
                else
                {
                    ImGui::Text( " \xE2\x80\x93 %s]", RealToString( var.maxlat ) );
                }
            }
            ImGui::SameLine();
            TextDisabledUnformatted( "(cycles in execution, may vary by used output)" );
        }
        TextFocused( "\xce\xbcops:", RealToString( var.uops ) );
        if( var.port != -1 ) TextFocused( "Ports:", PortList[var.port] );
        ImGui::Separator();
        TextFocused( "ISA set:", IsaList[var.isaSet] );
        TextDisabledUnformatted( "Operands:" );
        ImGui::SameLine();
        bool first = true;
        for( int i=0; i<var.descNum; i++ )
        {
            const char* t = "?";
            switch( var.desc[i].type )
            {
            case 0:
                t = "Imm";
                break;
            case 1:
                t = "Reg";
                break;
            case 2:
                t = var.desc[i].width == 0 ? "AGen" : "Mem";
                break;
            default:
                assert( false );
                break;
            }
            if( first )
            {
                first = false;
                if( var.desc[i].width == 0 )
                {
                    ImGui::TextUnformatted( t );
                }
                else
                {
                    ImGui::Text( "%s%i", t, var.desc[i].width );
                }
            }
            else
            {
                ImGui::SameLine( 0, 0 );
                if( var.desc[i].width == 0 )
                {
                    ImGui::Text( ", %s", t );
                }
                else
                {
                    ImGui::Text( ", %s%i", t, var.desc[i].width );
                }
            }
        }
        ImGui::EndTooltip();
        if( m_font ) ImGui::PushFont( m_font );
    }

    if( line.jumpAddr != 0 )
    {
        uint32_t offset = 0;
        const auto base = worker.GetSymbolForAddress( line.jumpAddr, offset );
        auto sym = base == 0 ? worker.GetSymbolData( line.jumpAddr ) : worker.GetSymbolData( base );
        if( sym )
        {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if( base == m_baseAddr )
            {
                ImGui::TextDisabled( "-> [%s+%" PRIu32"]", worker.GetString( sym->name ), offset );
                if( ImGui::IsItemHovered() )
                {
                    m_highlightAddr = line.jumpAddr;
                    if( ImGui::IsItemClicked() )
                    {
                        m_targetAddr = line.jumpAddr;
                        m_selectedAddresses.clear();
                        m_selectedAddresses.emplace( line.jumpAddr );
                    }
                }
            }
            else
            {
                ImGui::TextDisabled( "[%s+%" PRIu32"]", worker.GetString( sym->name ), offset );
                if( ImGui::IsItemClicked() ) jumpOut = line.jumpAddr;
            }
        }
    }

    if( lineHovered )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x11FFFFFF );
    }

    draw->AddLine( wpos + ImVec2( 0, ty+2 ), wpos + ImVec2( w, ty+2 ), 0x08FFFFFF );
}

void SourceView::SelectLine( uint32_t line, const Worker* worker, bool changeAsmLine, uint64_t targetAddr )
{
    m_selectedLine = line;
    if( m_symAddr == 0 ) return;
    assert( worker );
    SelectAsmLines( m_fileStringIdx, line, *worker, changeAsmLine, targetAddr );
}

void SourceView::SelectAsmLines( uint32_t file, uint32_t line, const Worker& worker, bool changeAsmLine, uint64_t targetAddr )
{
    m_selectedAddresses.clear();
    auto addresses = worker.GetAddressesForLocation( file, line );
    if( addresses )
    {
        const auto& addr = *addresses;
        if( changeAsmLine )
        {
            if( targetAddr != 0 )
            {
                m_targetAddr = targetAddr;
            }
            else
            {
                for( auto& v : addr )
                {
                    if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
                    {
                        m_targetAddr = v;
                        break;
                    }
                }
            }
        }
        for( auto& v : addr )
        {
            if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
            {
                m_selectedAddresses.emplace( v );
            }
        }
    }
}

void SourceView::SelectAsmLinesHover( uint32_t file, uint32_t line, const Worker& worker )
{
    assert( m_selectedAddressesHover.empty() );
    auto addresses = worker.GetAddressesForLocation( file, line );
    if( addresses )
    {
        for( auto& v : *addresses )
        {
            if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
            {
                m_selectedAddressesHover.emplace( v );
            }
        }
    }
}

void SourceView::GatherIpStats( uint64_t addr, uint32_t& iptotalSrc, uint32_t& iptotalAsm, unordered_flat_map<uint64_t, uint32_t>& ipcountSrc, unordered_flat_map<uint64_t, uint32_t>& ipcountAsm, uint32_t& ipmaxSrc, uint32_t& ipmaxAsm, const Worker& worker )
{
    auto ipmap = worker.GetSymbolInstructionPointers( addr );
    if( !ipmap ) return;
    for( auto& ip : *ipmap )
    {
        if( m_file )
        {
            auto frame = worker.GetCallstackFrame( ip.first );
            if( frame )
            {
                auto ffn = worker.GetString( frame->data[0].file );
                if( strcmp( ffn, m_file ) == 0 )
                {
                    const auto line = frame->data[0].line;
                    auto it = ipcountSrc.find( line );
                    if( it == ipcountSrc.end() )
                    {
                        ipcountSrc.emplace( line, ip.second );
                        if( ipmaxSrc < ip.second ) ipmaxSrc = ip.second;
                    }
                    else
                    {
                        const auto sum = it->second + ip.second;
                        it->second = sum;
                        if( ipmaxSrc < sum ) ipmaxSrc = sum;
                    }
                    iptotalSrc += ip.second;
                }
            }
        }

        auto addr = worker.GetCanonicalPointer( ip.first );
        assert( ipcountAsm.find( addr ) == ipcountAsm.end() );
        ipcountAsm.emplace( addr, ip.second );
        iptotalAsm += ip.second;
        if( ipmaxAsm < ip.second ) ipmaxAsm = ip.second;
    }
}

namespace {
static unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> GetKeywords()
{
    unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> ret;
    for( auto& v : {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
        "bitand", "bitor", "break", "case", "catch", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "for",
        "friend", "if", "inline", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "operator",
        "or", "or_eq", "private", "protected", "public", "reflexpr", "register", "reinterpret_cast",
        "return", "requires", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
        "synchronized", "template", "thread_local", "throw", "try", "typedef", "typeid", "typename",
        "union", "using", "virtual", "volatile", "while", "xor", "xor_eq", "override", "final", "import",
        "module", "transaction_safe", "transaction_safe_dynamic" } )
    {
        ret.insert( v );
    }
    return ret;
}
static unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> GetTypes()
{
    unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> ret;
    for( auto& v : {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short", "signed",
        "unsigned", "void", "wchar_t", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "int_fast8_t",
        "int_fast16_t", "int_fast32_t", "int_fast64_t", "int_least8_t", "int_least16_t", "int_least32_t",
        "int_least64_t", "intmax_t", "intptr_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "uint_fast8_t",
        "uint_fast16_t", "uint_fast32_t", "uint_fast64_t", "uint_least8_t", "uint_least16_t", "uint_least32_t",
        "uint_least64_t", "uintmax_t", "uintptr_t", "type_info", "bad_typeid", "bad_cast", "type_index",
        "clock_t", "time_t", "tm", "timespec", "ptrdiff_t", "nullptr_t", "max_align_t", "auto",

        "__m64", "__m128", "__m128i", "__m128d", "__m256", "__m256i", "__m256d", "__m512", "__m512i",
        "__m512d", "__mmask8", "__mmask16", "__mmask32", "__mmask64",

        "int8x8_t", "int16x4_t", "int32x2_t", "int64x1_t", "uint8x8_t", "uint16x4_t", "uint32x2_t",
        "uint64x1_t", "float32x2_t", "poly8x8_t", "poly16x4_t", "int8x16_t", "int16x8_t", "int32x4_t",
        "int64x2_t", "uint8x16_t", "uint16x8_t", "uint32x4_t", "uint64x2_t", "float32x4_t", "poly8x16_t",
        "poly16x8_t",

        "int8x8x2_t", "int16x4x2_t", "int32x2x2_t", "int64x1x2_t", "uint8x8x2_t", "uint16x4x2_t",
        "uint32x2x2_t", "uint64x1x2_t", "float32x2x2_t", "poly8x8x2_t", "poly16x4x2_t", "int8x16x2_t",
        "int16x8x2_t", "int32x4x2_t", "int64x2x2_t", "uint8x16x2_t", "uint16x8x2_t", "uint32x4x2_t",
        "uint64x2x2_t", "float32x4x2_t", "poly8x16x2_t", "poly16x8x2_t",

        "int8x8x3_t", "int16x4x3_t", "int32x2x3_t", "int64x1x3_t", "uint8x8x3_t", "uint16x4x3_t",
        "uint32x2x3_t", "uint64x1x3_t", "float32x2x3_t", "poly8x8x3_t", "poly16x4x3_t", "int8x16x3_t",
        "int16x8x3_t", "int32x4x3_t", "int64x2x3_t", "uint8x16x3_t", "uint16x8x3_t", "uint32x4x3_t",
        "uint64x2x3_t", "float32x4x3_t", "poly8x16x3_t", "poly16x8x3_t",

        "int8x8x4_t", "int16x4x4_t", "int32x2x4_t", "int64x1x4_t", "uint8x8x4_t", "uint16x4x4_t",
        "uint32x2x4_t", "uint64x1x4_t", "float32x2x4_t", "poly8x8x4_t", "poly16x4x4_t", "int8x16x4_t",
        "int16x8x4_t", "int32x4x4_t", "int64x2x4_t", "uint8x16x4_t", "uint16x8x4_t", "uint32x4x4_t",
        "uint64x2x4_t", "float32x4x4_t", "poly8x16x4_t", "poly16x8x4_t" } )
    {
        ret.insert( v );
    }
    return ret;
}
static unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> GetSpecial()
{
    unordered_flat_set<const char*, charutil::Hasher, charutil::Comparator> ret;
    for( auto& v : { "this", "nullptr", "true", "false", "goto", "NULL" } )
    {
        ret.insert( v );
    }
    return ret;
}
}

static const auto s_keywords = GetKeywords();
static const auto s_types = GetTypes();
static const auto s_special = GetSpecial();

static bool TokenizeNumber( const char*& begin, const char* end )
{
    const bool startNum = *begin >= '0' && *begin <= '9';
    if( *begin != '+' && *begin != '-' && !startNum ) return false;
    begin++;
    bool hasNum = startNum;
    while( begin < end && ( ( *begin >= '0' && *begin <= '9' ) || *begin == '\'' ) )
    {
        hasNum = true;
        begin++;
    }
    if( !hasNum ) return false;
    bool isFloat = false, isHex = false, isBinary = false;
    if( begin < end )
    {
        if( *begin == '.' )
        {
            isFloat = true;
            begin++;
            while( begin < end && ( ( *begin >= '0' && *begin <= '9' ) || *begin == '\'' ) ) begin++;
        }
        else if( *begin == 'x' || *begin == 'X' )
        {
            isHex = true;
            begin++;
            while( begin < end && ( ( *begin >= '0' && *begin <= '9' ) || ( *begin >= 'a' && *begin <= 'f' ) || ( *begin >= 'A' && *begin <= 'F' ) || *begin == '\'' ) ) begin++;
        }
        else if( *begin == 'b' || *begin == 'B' )
        {
            isBinary = true;
            begin++;
            while( begin < end && ( *begin == '0' || *begin == '1' ) || *begin == '\'' ) begin++;
        }
    }
    if( !isBinary )
    {
        if( begin < end && ( *begin == 'e' || *begin == 'E' || *begin == 'p' || *begin == 'P' ) )
        {
            isFloat = true;
            begin++;
            if( begin < end && ( *begin == '+' || *begin == '-' ) ) begin++;
            bool hasDigits = false;
            while( begin < end && ( ( *begin >= '0' && *begin <= '9' ) || ( *begin >= 'a' && *begin <= 'f' ) || ( *begin >= 'A' && *begin <= 'F' ) || *begin == '\'' ) )
            {
                hasDigits = true;
                begin++;
            }
            if( !hasDigits ) return false;
        }
        if( begin < end && ( *begin == 'f' || *begin == 'F' || *begin == 'l' || *begin == 'L' ) ) begin++;
    }
    if( !isFloat )
    {
        while( begin < end && ( *begin == 'u' || *begin == 'U' || *begin == 'l' || *begin == 'L' ) ) begin++;
    }
    return true;
}

SourceView::TokenColor SourceView::IdentifyToken( const char*& begin, const char* end )
{
    if( *begin == '"' )
    {
        begin++;
        while( begin < end )
        {
            if( *begin == '"' )
            {
                begin++;
                break;
            }
            begin += 1 + ( *begin == '\\' && end - begin > 1 && *(begin+1) == '"' );
        }
        return TokenColor::String;
    }
    if( *begin == '\'' )
    {
        begin++;
        if( begin < end && *begin == '\\' ) begin++;
        if( begin < end ) begin++;
        if( begin < end && *begin == '\'' ) begin++;
        return TokenColor::CharacterLiteral;
    }
    if( ( *begin >= 'a' && *begin <= 'z' ) || ( *begin >= 'A' && *begin <= 'Z' ) || *begin == '_' )
    {
        const char* tmp = begin;
        begin++;
        while( begin < end && ( *begin >= 'a' && *begin <= 'z' ) || ( *begin >= 'A' && *begin <= 'Z' ) || ( *begin >= '0' && *begin <= '9' ) || *begin == '_' ) begin++;
        if( begin - tmp <= 24 )
        {
            char buf[25];
            memcpy( buf, tmp, begin-tmp );
            buf[begin-tmp] = '\0';
            if( s_keywords.find( buf ) != s_keywords.end() ) return TokenColor::Keyword;
            if( s_types.find( buf ) != s_types.end() ) return TokenColor::Type;
            if( s_special.find( buf ) != s_special.end() ) return TokenColor::Special;
        }
        return TokenColor::Default;
    }
    const char* tmp = begin;
    if( TokenizeNumber( begin, end ) ) return TokenColor::Number;
    begin = tmp;
    if( *begin == '/' && end - begin > 1 )
    {
        if( *(begin+1) == '/' )
        {
            begin = end;
            return TokenColor::Comment;
        }
        if( *(begin+1) == '*' )
        {
            begin += 2;
            for(;;)
            {
                while( begin < end && *begin != '*' ) begin++;
                if( begin == end )
                {
                    m_tokenizer.isInComment = true;
                    return TokenColor::Comment;
                }
                begin++;
                if( begin < end && *begin == '/' )
                {
                    begin++;
                    return TokenColor::Comment;
                }
            }
        }
    }
    while( begin < end )
    {
        switch( *begin )
        {
        case '[':
        case ']':
        case '{':
        case '}':
        case '!':
        case '%':
        case '^':
        case '&':
        case '*':
        case '(':
        case ')':
        case '-':
        case '+':
        case '=':
        case '~':
        case '|':
        case '<':
        case '>':
        case '?':
        case ':':
        case '/':
        case ';':
        case ',':
        case '.':
            begin++;
            break;
        default:
            goto out;
        }
    }
out:
    if( begin != tmp ) return TokenColor::Punctuation;
    begin = end;
    return TokenColor::Default;
}

std::vector<SourceView::Token> SourceView::Tokenize( const char* begin, const char* end )
{
    std::vector<Token> ret;
    if( m_tokenizer.isInPreprocessor )
    {
        if( begin == end )
        {
            m_tokenizer.isInPreprocessor = false;
            return ret;
        }
        if( *(end-1) != '\\' ) m_tokenizer.isInPreprocessor = false;
        ret.emplace_back( Token { begin, end, TokenColor::Preprocessor } );
        return ret;
    }
    const bool first = !m_tokenizer.isInComment;
    while( begin != end )
    {
        if( m_tokenizer.isInComment )
        {
            const auto pos = begin;
            for(;;)
            {
                while( begin != end && *begin != '*' ) begin++;
                begin++;
                if( begin < end )
                {
                    if( *begin == '/' )
                    {
                        begin++;
                        ret.emplace_back( Token { pos, begin, TokenColor::Comment } );
                        m_tokenizer.isInComment = false;
                        break;
                    }
                }
                else
                {
                    ret.emplace_back( Token { pos, end, TokenColor::Comment } );
                    return ret;
                }
            }
        }
        else
        {
            while( begin != end && isspace( *begin ) ) begin++;
            if( first && begin < end && *begin == '#' )
            {
                if( *(end-1) == '\\' ) m_tokenizer.isInPreprocessor = true;
                ret.emplace_back( Token { begin, end, TokenColor::Preprocessor } );
                return ret;
            }
            const auto pos = begin;
            const auto col = IdentifyToken( begin, end );
            ret.emplace_back( Token { pos, begin, col } );
        }
    }
    return ret;
}

void SourceView::SelectMicroArchitecture( const char* moniker )
{
    int idx = 0;
    for( auto& v : s_uArchUx )
    {
        if( strcmp( v.moniker, moniker ) == 0 )
        {
            m_selMicroArch = idx;
            break;
        }
        idx++;
    }
    for( idx=0; idx<MicroArchitectureNum; idx++ )
    {
        if( strcmp( MicroArchitectureList[idx], moniker ) == 0 )
        {
            m_idxMicroArch = idx;
            break;
        }
    }
    assert( idx != MicroArchitectureNum );
}

}
