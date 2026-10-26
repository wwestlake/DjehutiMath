#include "MathFont.h"

#include <cmath>

namespace math_font
{
namespace
{
struct Reader
{
    const juce::uint8* data;
    size_t size;
    juce::uint16 u16(size_t at) const { return at + 2 <= size ? (juce::uint16) ((data[at] << 8) | data[at + 1]) : (juce::uint16) 0; }
    juce::int16 s16(size_t at) const { return (juce::int16) u16(at); }
    juce::uint32 u32(size_t at) const
    {
        return at + 4 <= size ? ((juce::uint32) data[at] << 24) | ((juce::uint32) data[at + 1] << 16) | ((juce::uint32) data[at + 2] << 8) | data[at + 3] : 0u;
    }
};

// glyph -> index into the table that the coverage belongs to
std::unordered_map<std::uint16_t, int> readCoverage(const Reader& r, size_t at)
{
    std::unordered_map<std::uint16_t, int> map;
    const auto format = r.u16(at);
    if (format == 1)
    {
        const int n = r.u16(at + 2);
        for (int i = 0; i < n; ++i)
            map[r.u16(at + 4 + 2 * (size_t) i)] = i;
    }
    else if (format == 2)
    {
        const int n = r.u16(at + 2);
        for (int i = 0; i < n; ++i)
        {
            const size_t rec = at + 4 + 6 * (size_t) i;
            const int first = r.u16(rec), last = r.u16(rec + 2), start = r.u16(rec + 4);
            for (int g = first; g <= last; ++g)
                map[(std::uint16_t) g] = start + (g - first);
        }
    }
    return map;
}
}

juce::File MathFont::overrideFile;

juce::File bundledFontFile()
{
    const auto beside = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("fonts").getChildFile("STIXTwoMath-Regular.otf");
    if (beside.existsAsFile())
        return beside;
#ifdef DJEHUTIMATH_SOURCE_ROOT
    return juce::File(DJEHUTIMATH_SOURCE_ROOT).getChildFile("Resources").getChildFile("fonts").getChildFile("STIXTwoMath-Regular.otf");
#else
    return beside;
#endif
}


// ---- CFF (Type 2 charstrings) -----------------------------------------------------------------------------------------------
struct MathFont::Cff
{
    using Range = std::pair<size_t, size_t>;       // offset, length within the font file
    struct Private { std::vector<Range> subrs; };
    std::vector<Range> charStrings, globalSubrs;
    std::vector<Private> privates;                 // one per font dict; a plain CFF has exactly one
    std::vector<int> fdSelect;                     // glyph -> private index; empty means all zero
};

namespace
{
using Range = std::pair<size_t, size_t>;

bool readIndex(const Reader& r, size_t at, std::vector<Range>& out, size_t& end)
{
    if (at + 2 > r.size) return false;
    const int count = r.u16(at);
    if (count == 0) { end = at + 2; return true; }
    if (at + 3 > r.size) return false;
    const int offSize = r.data[at + 2];
    if (offSize < 1 || offSize > 4) return false;
    const size_t table = at + 3, base = table + (size_t) (count + 1) * (size_t) offSize - 1;
    if (base > r.size) return false;
    auto offset = [&](int i) {
        size_t v = 0;
        for (int k = 0; k < offSize; ++k) v = (v << 8) | r.data[table + (size_t) i * (size_t) offSize + (size_t) k];
        return v;
    };
    for (int i = 0; i < count; ++i)
    {
        const size_t a = offset(i), b = offset(i + 1);
        if (b < a || base + b > r.size) return false;
        out.push_back({ base + a, b - a });
    }
    end = base + offset(count);
    return true;
}

// DICT data: operator (two-byte operators are 1200 + second byte) -> operands
std::map<int, std::vector<double>> readDict(const Reader& r, size_t at, size_t length)
{
    std::map<int, std::vector<double>> out;
    std::vector<double> operands;
    const size_t end = std::min(at + length, r.size);
    while (at < end)
    {
        const int b0 = r.data[at++];
        if (b0 <= 21)
        {
            int op = b0;
            if (b0 == 12 && at < end) op = 1200 + r.data[at++];
            out[op] = operands;
            operands.clear();
        }
        else if (b0 == 28 && at + 2 <= end) { operands.push_back((double) r.s16(at)); at += 2; }
        else if (b0 == 29 && at + 4 <= end) { operands.push_back((double) (juce::int32) r.u32(at)); at += 4; }
        else if (b0 == 30)
        {
            // real number: nibbles until 0xF; its value is never needed for outlines, but it must be skipped
            while (at < end)
            {
                const int byte = r.data[at++];
                if ((byte & 0xF) == 0xF || (byte >> 4) == 0xF) break;
            }
            operands.push_back(0.0);
        }
        else if (b0 >= 32 && b0 <= 246) operands.push_back((double) (b0 - 139));
        else if (b0 >= 247 && b0 <= 250 && at < end) operands.push_back((double) ((b0 - 247) * 256 + r.data[at++] + 108));
        else if (b0 >= 251 && b0 <= 254 && at < end) operands.push_back((double) (-(b0 - 251) * 256 - r.data[at++] - 108));
        else break;
    }
    return out;
}

int subrBias(size_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

// Runs one glyph's charstring and builds its outline in em units (y down, baseline at zero).
class CharstringRunner
{
public:
    CharstringRunner(const Reader& reader, const std::vector<Range>& globals, const std::vector<Range>& locals, float unitsPerEm)
        : r(reader), globalSubrs(globals), localSubrs(locals), scale(1.0f / unitsPerEm) {}

    juce::Path run(const Range& code)
    {
        execute(code, 0);
        if (open) path.closeSubPath();
        return path;
    }

private:
    const Reader& r;
    const std::vector<Range>& globalSubrs;
    const std::vector<Range>& localSubrs;
    float scale;
    juce::Path path;
    std::vector<double> st;
    double x = 0, y = 0;
    bool open = false, widthTaken = false, finished = false;
    int stems = 0;

    float X(double v) const { return (float) v * scale; }
    float Y(double v) const { return -(float) v * scale; }
    void takeWidth(bool present) { if (!widthTaken) { widthTaken = true; if (present && !st.empty()) st.erase(st.begin()); } }
    void moveBy(double dx, double dy)
    {
        if (open) path.closeSubPath();
        x += dx; y += dy;
        path.startNewSubPath(X(x), Y(y));
        open = true;
    }
    void ensureOpen() { if (!open) { path.startNewSubPath(X(x), Y(y)); open = true; } }
    void lineBy(double dx, double dy) { ensureOpen(); x += dx; y += dy; path.lineTo(X(x), Y(y)); }
    void curveBy(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3)
    {
        ensureOpen();
        const double x1 = x + dx1, y1 = y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
        x = x2 + dx3; y = y2 + dy3;
        path.cubicTo(X(x1), Y(y1), X(x2), Y(y2), X(x), Y(y));
    }

    void execute(const Range& code, int depth)
    {
        if (depth > 10) { finished = true; return; }
        size_t pc = code.first;
        const size_t end = code.first + code.second;
        while (pc < end && !finished)
        {
            const int b0 = r.data[pc++];
            if (b0 >= 32 || b0 == 28)
            {
                if (b0 == 28) { st.push_back((double) r.s16(pc)); pc += 2; }
                else if (b0 <= 246) st.push_back((double) (b0 - 139));
                else if (b0 <= 250) st.push_back((double) ((b0 - 247) * 256 + r.data[pc++] + 108));
                else if (b0 <= 254) st.push_back((double) (-(b0 - 251) * 256 - r.data[pc++] - 108));
                else { st.push_back((double) (juce::int32) r.u32(pc) / 65536.0); pc += 4; }
                if (st.size() > 96) { finished = true; return; }
                continue;
            }
            switch (b0)
            {
                case 1: case 3: case 18: case 23:                       // hstem vstem hstemhm vstemhm
                    takeWidth(st.size() % 2 == 1); stems += (int) st.size() / 2; st.clear(); break;
                case 19: case 20:                                       // hintmask cntrmask
                    takeWidth(st.size() % 2 == 1); stems += (int) st.size() / 2; st.clear(); pc += (size_t) (stems + 7) / 8; break;
                case 21: takeWidth(st.size() > 2); if (st.size() >= 2) moveBy(st[0], st[1]); st.clear(); break;
                case 22: takeWidth(st.size() > 1); if (!st.empty()) moveBy(st[0], 0); st.clear(); break;
                case 4: takeWidth(st.size() > 1); if (!st.empty()) moveBy(0, st[0]); st.clear(); break;
                case 5: for (size_t i = 0; i + 1 < st.size(); i += 2) lineBy(st[i], st[i + 1]); st.clear(); break;
                case 6: case 7:                                         // hlineto vlineto: alternate
                {
                    bool horizontal = b0 == 6;
                    for (double v : st) { if (horizontal) lineBy(v, 0); else lineBy(0, v); horizontal = !horizontal; }
                    st.clear();
                    break;
                }
                case 8: for (size_t i = 0; i + 5 < st.size(); i += 6) curveBy(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]); st.clear(); break;
                case 24:                                                // rcurveline
                {
                    size_t i = 0;
                    for (; i + 7 < st.size(); i += 6) curveBy(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
                    if (i + 1 < st.size()) lineBy(st[i], st[i + 1]);
                    st.clear();
                    break;
                }
                case 25:                                                // rlinecurve
                {
                    size_t i = 0;
                    for (; i + 7 < st.size(); i += 2) lineBy(st[i], st[i + 1]);
                    if (i + 5 < st.size()) curveBy(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
                    st.clear();
                    break;
                }
                case 26:                                                // vvcurveto
                {
                    size_t i = 0;
                    double dx1 = 0;
                    if (st.size() % 4 == 1) dx1 = st[i++];
                    for (; i + 3 < st.size(); i += 4) { curveBy(dx1, st[i], st[i + 1], st[i + 2], 0, st[i + 3]); dx1 = 0; }
                    st.clear();
                    break;
                }
                case 27:                                                // hhcurveto
                {
                    size_t i = 0;
                    double dy1 = 0;
                    if (st.size() % 4 == 1) dy1 = st[i++];
                    for (; i + 3 < st.size(); i += 4) { curveBy(st[i], dy1, st[i + 1], st[i + 2], st[i + 3], 0); dy1 = 0; }
                    st.clear();
                    break;
                }
                case 30: case 31:                                       // vhcurveto hvcurveto
                {
                    bool horizontal = b0 == 31;
                    for (size_t i = 0; i + 3 < st.size(); i += 4)
                    {
                        const bool last = st.size() - i == 5;
                        const double extra = last ? st[i + 4] : 0.0;
                        if (horizontal) curveBy(st[i], 0, st[i + 1], st[i + 2], extra, st[i + 3]);
                        else curveBy(0, st[i], st[i + 1], st[i + 2], st[i + 3], extra);
                        horizontal = !horizontal;
                    }
                    st.clear();
                    break;
                }
                case 10: case 29:                                       // callsubr callgsubr
                {
                    if (st.empty()) { finished = true; return; }
                    const auto& list = b0 == 10 ? localSubrs : globalSubrs;
                    const long index = (long) st.back() + subrBias(list.size());
                    st.pop_back();
                    if (index < 0 || (size_t) index >= list.size()) { finished = true; return; }
                    execute(list[(size_t) index], depth + 1);
                    break;
                }
                case 11: return;                                        // return
                case 14: takeWidth(st.size() == 1 || st.size() == 5); st.clear(); finished = true; return;   // endchar
                case 12:
                {
                    const int b1 = pc < end ? r.data[pc++] : 0;
                    if (b1 == 34 && st.size() >= 7)                     // hflex
                    {
                        curveBy(st[0], 0, st[1], st[2], st[3], 0);
                        curveBy(st[4], 0, st[5], -st[2], st[6], 0);
                    }
                    else if (b1 == 35 && st.size() >= 12)               // flex
                    {
                        curveBy(st[0], st[1], st[2], st[3], st[4], st[5]);
                        curveBy(st[6], st[7], st[8], st[9], st[10], st[11]);
                    }
                    else if (b1 == 36 && st.size() >= 9)                // hflex1
                    {
                        curveBy(st[0], st[1], st[2], st[3], st[4], 0);
                        curveBy(st[5], 0, st[6], st[7], st[8], -(st[1] + st[3] + st[7]));
                    }
                    else if (b1 == 37 && st.size() >= 11)               // flex1
                    {
                        const double dx = st[0] + st[2] + st[4] + st[6] + st[8], dy = st[1] + st[3] + st[5] + st[7] + st[9];
                        curveBy(st[0], st[1], st[2], st[3], st[4], st[5]);
                        if (std::abs(dx) > std::abs(dy)) curveBy(st[6], st[7], st[8], st[9], st[10], -dy);
                        else curveBy(st[6], st[7], st[8], st[9], -dx, st[10]);
                    }
                    st.clear();
                    break;
                }
                default: st.clear(); break;                             // unsupported operators are ignored
            }
        }
    }
};
}

MathFont::~MathFont() = default;

MathFont::MathFont()
{
    load(overrideFile != juce::File() ? overrideFile : bundledFontFile());
}

void MathFont::useFile(const juce::File& file) { overrideFile = file; }

MathFont& MathFont::instance()
{
    static MathFont font;
    return font;
}

bool MathFont::load(const juce::File& file)
{
    if (!file.existsAsFile() || !file.loadFileAsData(fileData) || fileData.getSize() < 1000)
    {
        fail("The math font " + file.getFullPathName() + " is missing.");
        return false;
    }
    const Reader r { static_cast<const juce::uint8*>(fileData.getData()), fileData.getSize() };
    if (r.u32(0) != 0x4F54544F && r.u32(0) != 0x00010000)   // 'OTTO' or TrueType
    {
        fail("The math font file is not an OpenType font.");
        return false;
    }
    std::map<std::string, std::pair<size_t, size_t>> tables;
    const int numTables = r.u16(4);
    for (int i = 0; i < numTables; ++i)
    {
        const size_t rec = 12 + 16 * (size_t) i;
        std::string tag((const char*) r.data + rec, 4);
        tables[tag] = { r.u32(rec + 8), r.u32(rec + 12) };
    }
    for (const char* needed : { "head", "hhea", "hmtx", "maxp", "cmap", "MATH", "CFF " })
        if (tables.count(needed) == 0)
        {
            fail(juce::String("The math font has no '") + needed + "' table.");
            return false;
        }
    upem = std::max<int>(16, r.u16(tables["head"].first + 18));

    // advances
    {
        const int numMetrics = r.u16(tables["hhea"].first + 34);
        const int numGlyphs = r.u16(tables["maxp"].first + 4);
        advances.assign((size_t) numGlyphs, 0);
        const auto hmtx = tables["hmtx"].first;
        int last = 0;
        for (int g = 0; g < numGlyphs; ++g)
        {
            if (g < numMetrics)
                last = r.u16(hmtx + 4 * (size_t) g);
            advances[(size_t) g] = last;
        }
    }

    // character map: prefer Unicode full repertoire (3,10 format 12), else BMP (3,1 format 4)
    {
        const auto base = tables["cmap"].first;
        const int count = r.u16(base + 2);
        size_t full = 0, bmp = 0;
        for (int i = 0; i < count; ++i)
        {
            const size_t rec = base + 4 + 8 * (size_t) i;
            const int platform = r.u16(rec), encoding = r.u16(rec + 2);
            const size_t offset = base + r.u32(rec + 4);
            if (platform == 3 && encoding == 10) full = offset;
            if (platform == 3 && encoding == 1) bmp = offset;
            if (platform == 0 && r.u16(offset) == 12 && full == 0) full = offset;
        }
        if (full != 0 && r.u16(full) == 12)
        {
            const auto groups = r.u32(full + 12);
            for (juce::uint32 g = 0; g < groups && g < 200000; ++g)
            {
                const size_t rec = full + 16 + 12 * (size_t) g;
                const auto first = r.u32(rec), last = r.u32(rec + 4), start = r.u32(rec + 8);
                if (last < first || last - first > 0x20000) continue;
                for (juce::uint32 c = first; c <= last; ++c)
                    cmap[(juce::juce_wchar) c] = (std::uint16_t) (start + (c - first));
            }
        }
        else if (bmp != 0 && r.u16(bmp) == 4)
        {
            const int segX2 = r.u16(bmp + 6), segs = segX2 / 2;
            const size_t endCodes = bmp + 14, startCodes = endCodes + (size_t) segX2 + 2, deltas = startCodes + (size_t) segX2,
                         rangeOffsets = deltas + (size_t) segX2;
            for (int s = 0; s < segs; ++s)
            {
                const int end = r.u16(endCodes + 2 * (size_t) s), start = r.u16(startCodes + 2 * (size_t) s);
                const int delta = r.s16(deltas + 2 * (size_t) s), range = r.u16(rangeOffsets + 2 * (size_t) s);
                for (int c = start; c <= end && c != 0xFFFF; ++c)
                {
                    int glyph = 0;
                    if (range == 0) glyph = (c + delta) & 0xFFFF;
                    else
                    {
                        const auto at = rangeOffsets + 2 * (size_t) s + (size_t) range + 2 * (size_t) (c - start);
                        glyph = r.u16(at);
                        if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
                    }
                    if (glyph != 0) cmap[(juce::juce_wchar) c] = (std::uint16_t) glyph;
                }
            }
        }
        else
        {
            fail("The math font has no usable character map.");
            return false;
        }
    }

    // MATH table
    {
        const auto math = tables["MATH"].first;
        if (r.u32(math) != 0x00010000)
        {
            fail("Unsupported MATH table version.");
            return false;
        }
        const auto constantsAt = math + r.u16(math + 4);
        mathConstants.scriptPercentScaleDown = r.s16(constantsAt);
        mathConstants.scriptScriptPercentScaleDown = r.s16(constantsAt + 2);
        mathConstants.delimitedSubFormulaMinHeight = r.u16(constantsAt + 4);
        mathConstants.displayOperatorMinHeight = r.u16(constantsAt + 6);
        for (int i = 0; i < ConstantCount; ++i)
            mathConstants.value[i] = r.s16(constantsAt + 8 + 4 * (size_t) i);
        mathConstants.radicalDegreeBottomRaisePercent = r.s16(constantsAt + 8 + 4 * (size_t) ConstantCount);

        if (const auto infoOffset = r.u16(math + 6); infoOffset != 0)
        {
            const auto info = math + infoOffset;
            if (const auto italicsOffset = r.u16(info); italicsOffset != 0)
            {
                const auto table = info + italicsOffset;
                const auto coverage = readCoverage(r, table + r.u16(table));
                const int count = r.u16(table + 2);
                for (const auto& [glyph, index] : coverage)
                    if (index < count)
                        italics[glyph] = r.s16(table + 4 + 4 * (size_t) index);
            }
        }
        if (const auto variantsOffset = r.u16(math + 8); variantsOffset != 0)
        {
            const auto variants = math + variantsOffset;
            connectorOverlap = r.u16(variants);
            const auto coverage = readCoverage(r, variants + r.u16(variants + 2));
            const int vertCount = r.u16(variants + 6);
            const auto constructionOffsets = variants + 10;
            for (const auto& [glyph, index] : coverage)
            {
                if (index >= vertCount) continue;
                const auto construction = variants + r.u16(constructionOffsets + 2 * (size_t) index);
                Construction c;
                const int variantCount = r.u16(construction + 2);
                for (int v = 0; v < variantCount && v < 64; ++v)
                    c.variants.push_back({ r.u16(construction + 4 + 4 * (size_t) v), r.u16(construction + 6 + 4 * (size_t) v) });
                if (const auto assemblyOffset = r.u16(construction); assemblyOffset != 0)
                {
                    const auto assembly = construction + assemblyOffset;
                    const int partCount = r.u16(assembly + 4);
                    for (int p = 0; p < partCount && p < 16; ++p)
                    {
                        const auto rec = assembly + 6 + 10 * (size_t) p;
                        c.parts.push_back({ r.u16(rec), r.u16(rec + 2), r.u16(rec + 4), r.u16(rec + 6), (r.u16(rec + 8) & 1) != 0 });
                    }
                }
                vertical[glyph] = std::move(c);
            }
        }
    }

    // glyph outlines: the CFF table
    {
        auto parsed = std::make_unique<Cff>();
        const auto base = tables["CFF "].first;
        const size_t hdrSize = r.data[base + 2];
        size_t cursor = base + hdrSize, end = 0;
        std::vector<Range> names, tops, strings;
        if (!readIndex(r, cursor, names, end) || !readIndex(r, end, tops, end) || !readIndex(r, end, strings, end)
            || !readIndex(r, end, parsed->globalSubrs, end) || tops.empty())
        {
            fail("The math font's CFF table could not be read.");
            return false;
        }
        const auto top = readDict(r, tops[0].first, tops[0].second);
        const auto charStringsAt = top.find(17);
        if (charStringsAt == top.end() || charStringsAt->second.empty() || !readIndex(r, base + (size_t) charStringsAt->second[0], parsed->charStrings, end))
        {
            fail("The math font has no CFF charstrings.");
            return false;
        }
        auto readPrivate = [&](const std::map<int, std::vector<double>>& dict) {
            Cff::Private priv;
            if (const auto found = dict.find(18); found != dict.end() && found->second.size() >= 2)
            {
                const size_t size = (size_t) found->second[0], offset = base + (size_t) found->second[1];
                const auto privateDict = readDict(r, offset, size);
                if (const auto subrs = privateDict.find(19); subrs != privateDict.end() && !subrs->second.empty())
                {
                    size_t ignored = 0;
                    readIndex(r, offset + (size_t) subrs->second[0], priv.subrs, ignored);
                }
            }
            return priv;
        };
        if (const auto fdArray = top.find(1236); fdArray != top.end() && !fdArray->second.empty())
        {
            std::vector<Range> fonts;
            readIndex(r, base + (size_t) fdArray->second[0], fonts, end);
            for (const auto& f : fonts) parsed->privates.push_back(readPrivate(readDict(r, f.first, f.second)));
            if (const auto sel = top.find(1237); sel != top.end() && !sel->second.empty())
            {
                const size_t at = base + (size_t) sel->second[0];
                const size_t glyphs = parsed->charStrings.size();
                parsed->fdSelect.assign(glyphs, 0);
                if (r.data[at] == 0)
                    for (size_t g = 0; g < glyphs; ++g) parsed->fdSelect[g] = r.data[at + 1 + g];
                else if (r.data[at] == 3)
                {
                    const int ranges = r.u16(at + 1);
                    for (int i = 0; i < ranges; ++i)
                    {
                        const size_t rec = at + 3 + 3 * (size_t) i;
                        const size_t first = r.u16(rec), next = r.u16(rec + 3);
                        for (size_t g = first; g < next && g < glyphs; ++g) parsed->fdSelect[g] = r.data[rec + 2];
                    }
                }
            }
        }
        else
            parsed->privates.push_back(readPrivate(top));
        if (parsed->privates.empty()) parsed->privates.push_back({});
        cff = std::move(parsed);
    }
    loaded = true;
    return true;
}

std::uint16_t MathFont::glyphFor(juce::juce_wchar codepoint) const
{
    const auto found = cmap.find(codepoint);
    return found == cmap.end() ? (std::uint16_t) 0 : found->second;
}

int MathFont::advance(std::uint16_t glyph) const { return glyph < advances.size() ? advances[glyph] : 0; }

int MathFont::italicCorrection(std::uint16_t glyph) const
{
    const auto found = italics.find(glyph);
    return found == italics.end() ? 0 : found->second;
}

const Construction* MathFont::verticalConstruction(std::uint16_t glyph) const
{
    const auto found = vertical.find(glyph);
    return found == vertical.end() ? nullptr : &found->second;
}

const juce::Path& MathFont::outline(std::uint16_t glyph)
{
    auto found = paths.find(glyph);
    if (found != paths.end())
        return found->second;
    juce::Path path;
    if (cff != nullptr && glyph < cff->charStrings.size())
    {
        const Reader r { static_cast<const juce::uint8*>(fileData.getData()), fileData.getSize() };
        const auto fd = glyph < cff->fdSelect.size() ? (size_t) cff->fdSelect[glyph] : (size_t) 0;
        const auto& priv = cff->privates[std::min(fd, cff->privates.size() - 1)];
        CharstringRunner runner(r, cff->globalSubrs, priv.subrs, (float) upem);
        path = runner.run(cff->charStrings[glyph]);
    }
    bounds[glyph] = path.getBounds();
    return paths.emplace(glyph, std::move(path)).first->second;
}

juce::Rectangle<float> MathFont::inkBounds(std::uint16_t glyph)
{
    outline(glyph);
    return bounds[glyph];
}
}
