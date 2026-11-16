// Make 1-bit bitmap font tables (C files) for the gfx component.
// The glyphs are drawn with the same libraries and sizes as trmnl-server.
// Then a 50 % threshold makes them black and white, as the server does for the 1-bit screen.
//
// Usage: dotnet run --project tools/fontgen -- tools/fontgen/fonts.json

using System.Globalization;
using System.Text;
using System.Text.Json;
using SixLabors.Fonts;
using SixLabors.Fonts.Unicode;
using SixLabors.Fonts.WellKnownIds;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.Drawing.Processing;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Processing;

var configPath = Path.GetFullPath(args.Length > 0 ? args[0] : "fonts.json");
var baseDir = Path.GetDirectoryName(configPath)!;
var config = JsonSerializer.Deserialize<Config>(
    File.ReadAllText(configPath),
    new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
    ?? throw new InvalidOperationException("The config file is empty");

var collection = new FontCollection();
FontFamily? family = null;
foreach (var file in config.Files)
{
    family = collection.Add(Path.Combine(baseDir, file), out var description);
    Console.WriteLine($"{file}: {description.FontNameInvariantCulture}, " +
        description.GetNameById(CultureInfo.InvariantCulture, KnownNameIds.LicenseDescription));
}
if (family is null)
    throw new InvalidOperationException("No font files in the config");

var outDir = Path.GetFullPath(Path.Combine(baseDir, config.Output));
var fontsDir = Path.Combine(outDir, "fonts");
Directory.CreateDirectory(fontsDir);

foreach (var spec in config.Fonts)
{
    var style = Enum.Parse<FontStyle>(spec.Style, ignoreCase: true);
    var font = family.Value.CreateFont(spec.Size, style);
    var codes = Expand(config.Charsets[spec.Charset]);
    var c = Generate(font, spec, codes);
    File.WriteAllText(Path.Combine(fontsDir, spec.Name + ".c"), c, new UTF8Encoding(false));
}

File.WriteAllText(Path.Combine(outDir, "include", "gfx_fonts.h"), Header(config.Fonts), new UTF8Encoding(false));
Console.WriteLine($"Wrote {config.Fonts.Count} fonts to {fontsDir}");

static List<int> Expand(List<int[]> ranges)
{
    var codes = new SortedSet<int>();
    foreach (var r in ranges)
        for (var c = r[0]; c <= r[^1]; c++)
            codes.Add(c);
    return codes.ToList();
}

static string Generate(Font font, FontSpec spec, List<int> codes)
{
    var metrics = font.FontMetrics;
    var scale = (float)spec.Size / metrics.UnitsPerEm;
    var ascent = metrics.HorizontalMetrics.Ascender * scale;
    var descent = -metrics.HorizontalMetrics.Descender * scale;

    // trmnl-server draws a text at Origin = (x, baseline - ascent) with VerticalAlignment.Top.
    var pad = (int)MathF.Ceiling(spec.Size * 0.5f) + 2;
    var width = (int)MathF.Ceiling(spec.Size * 2.5f) + 2 * pad;
    var height = (int)MathF.Ceiling(ascent + descent) + 2 * pad;
    var baseline = pad + ascent;

    var bitmaps = new List<byte>();
    var glyphs = new StringBuilder();
    var count = 0;
    var missing = new List<string>();

    foreach (var code in codes)
    {
        // Glyph 0 is ".notdef": the font has no glyph for this character.
        if (!font.TryGetGlyphs(new CodePoint(code), out var found) || found.Count == 0
            || found[0].GlyphMetrics.GlyphId == 0)
        {
            missing.Add($"U+{code:X4}");
            continue;
        }

        var text = char.ConvertFromUtf32(code);
        var advance = TextMeasurer.MeasureAdvance(text, new TextOptions(font)).Width;

        using var image = new Image<L8>(width, height, new L8(255));
        image.Mutate(ctx => ctx.DrawText(new RichTextOptions(font)
        {
            Origin = new PointF(pad, pad),
            VerticalAlignment = VerticalAlignment.Top,
        }, text, Color.Black));

        // The same threshold as BinaryThreshold(0.5f): a pixel darker than the middle is black.
        int minX = int.MaxValue, minY = int.MaxValue, maxX = -1, maxY = -1;
        for (var y = 0; y < height; y++)
            for (var x = 0; x < width; x++)
                if (image[x, y].PackedValue < 128)
                {
                    minX = Math.Min(minX, x);
                    maxX = Math.Max(maxX, x);
                    minY = Math.Min(minY, y);
                    maxY = Math.Max(maxY, y);
                }

        int w = 0, h = 0, dx = 0, dy = 0;
        var offset = bitmaps.Count;
        if (maxX >= 0)
        {
            w = maxX - minX + 1;
            h = maxY - minY + 1;
            dx = minX - pad;
            dy = minY - (int)MathF.Round(baseline);
            if (minX == 0 || minY == 0 || maxX == width - 1 || maxY == height - 1)
                Console.WriteLine($"Warning: {spec.Name} U+{code:X4} touches the canvas edge");

            var bit = 0;
            byte current = 0;
            for (var y = minY; y <= maxY; y++)
                for (var x = minX; x <= maxX; x++)
                {
                    if (image[x, y].PackedValue < 128)
                        current |= (byte)(0x80 >> (bit & 7));
                    bit++;
                    if ((bit & 7) == 0)
                    {
                        bitmaps.Add(current);
                        current = 0;
                    }
                }
            if ((bit & 7) != 0)
                bitmaps.Add(current);
        }

        var adv16 = (int)MathF.Round(advance * 16);
        glyphs.Append(CultureInfo.InvariantCulture,
            $"    {{ 0x{code:X4}, {offset}, {adv16}, {w}, {h}, {dx}, {dy} }}, // U+{code:X4}\r\n");
        count++;
    }

    if (missing.Count > 0)
        Console.WriteLine($"{spec.Name}: no glyph for {string.Join(", ", missing)}");
    Console.WriteLine($"{spec.Name}: {count} glyphs, {bitmaps.Count} bytes");

    var sb = new StringBuilder();
    sb.Append($"// Generated by tools/fontgen: {spec.Style} {spec.Size} px. Do not edit.\r\n");
    sb.Append("#include \"gfx.h\"\r\n\r\n");
    sb.Append("static const uint8_t bitmaps[] = {\r\n");
    for (var i = 0; i < bitmaps.Count; i += 16)
    {
        sb.Append("    ");
        sb.Append(string.Join(", ", bitmaps.Skip(i).Take(16).Select(b => $"0x{b:X2}")));
        sb.Append(",\r\n");
    }
    if (bitmaps.Count == 0)
        sb.Append("    0x00,\r\n");
    sb.Append("};\r\n\r\n");
    sb.Append("static const gfx_glyph_t glyphs[] = {\r\n");
    sb.Append(glyphs);
    sb.Append("};\r\n\r\n");
    sb.Append($"const gfx_font_t {spec.Name} = {{\r\n");
    sb.Append("    .bitmaps = bitmaps,\r\n");
    sb.Append("    .glyphs = glyphs,\r\n");
    sb.Append($"    .count = {count},\r\n");
    sb.Append(CultureInfo.InvariantCulture, $"    .size = {spec.Size},\r\n");
    sb.Append($"    .ascent = {(int)MathF.Round(ascent)},\r\n");
    sb.Append($"    .descent = {(int)MathF.Round(descent)},\r\n");
    sb.Append("};\r\n");
    return sb.ToString();
}

static string Header(List<FontSpec> fonts)
{
    var sb = new StringBuilder();
    sb.Append("// Generated by tools/fontgen. Do not edit.\r\n");
    sb.Append("#pragma once\r\n\r\n");
    sb.Append("#include \"gfx.h\"\r\n\r\n");
    sb.Append("#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n");
    foreach (var f in fonts)
        sb.Append($"extern const gfx_font_t {f.Name}; // {f.Style} {f.Size} px\r\n");
    sb.Append("\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n");
    return sb.ToString();
}

sealed class Config
{
    public string Output { get; set; } = "";
    public List<string> Files { get; set; } = [];
    public Dictionary<string, List<int[]>> Charsets { get; set; } = [];
    public List<FontSpec> Fonts { get; set; } = [];
}

sealed class FontSpec
{
    public string Name { get; set; } = "";
    public string Style { get; set; } = "Regular";
    public int Size { get; set; }
    public string Charset { get; set; } = "text";
}
