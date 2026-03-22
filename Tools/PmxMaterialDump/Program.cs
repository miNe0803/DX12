// PMX 2.0/2.1: ヘッダ〜マテリアルまで読み、テクスチャ一覧とマテリアルを Markdown で出力。
// 参照: benikabocha/saba PMXFile.cpp (ReadHeader, ReadVertex, ReadFace, ReadTexture, ReadMaterial)

using System.Text;

if (args.Length < 1)
{
    Console.Error.WriteLine("Usage: PmxMaterialDump <path.pmx>");
    return 1;
}

var path = Path.GetFullPath(args[0]);
if (!File.Exists(path))
{
    Console.Error.WriteLine("File not found: " + path);
    return 1;
}

using var fs = File.OpenRead(path);
using var br = new BinaryReader(fs);

try
{
    var hdr = ReadHeader(br);
    ReadInfo(br, hdr);
    SkipVertices(br, hdr);
    SkipFaces(br, hdr);
    var textures = ReadTextures(br, hdr);
    var materials = ReadMaterials(br, hdr);

    var rel = args[0];
    Console.WriteLine("# PMX テクスチャ・マテリアル一覧");
    Console.WriteLine();
    Console.WriteLine($"**ファイル:** `{rel}`  ");
    Console.WriteLine($"**絶対パス:** `{path}`  ");
    Console.WriteLine();
    Console.WriteLine($"**PMX バージョン:** {hdr.Version:F2}  ");
    Console.WriteLine($"**文字エンコード:** {(hdr.Encode == 0 ? "UTF-16LE" : "UTF-8")}  ");
    Console.WriteLine($"**追加UV数 (addUV):** {hdr.AddUVNum}  ");
    Console.WriteLine($"**インデックス幅 (byte):** vertex={hdr.VertexIndexSize}, texture={hdr.TextureIndexSize}, material={hdr.MaterialIndexSize}, bone={hdr.BoneIndexSize}");
    Console.WriteLine();
    Console.WriteLine("## テクスチャ一覧 (index → パス)");
    Console.WriteLine();
    for (int i = 0; i < textures.Count; i++)
        Console.WriteLine($"- **[{i}]** `{textures[i]}`");
    Console.WriteLine();
    Console.WriteLine("## マテリアル一覧");
    Console.WriteLine();
    Console.WriteLine("SphereMode: 0=無効, 1=乗算(sph), 2=加算(spa), 3=サブテクスチャ(追加UV参照) など（モデルにより解釈）");
    Console.WriteLine();
    Console.WriteLine("ToonMode: **0** = テクスチャ参照（`toonTextureIndex` はテクスチャ表のインデックス） / **1** = 共有トゥーン（`toonTextureIndex` は 0〜9 の番号）");
    Console.WriteLine();
    Console.WriteLine("| # | 名前 | Diffuse RGBA | メインTex | スフィアTex | SphMode | ToonMode | Toon参照 | 面頂点数 |");
    Console.WriteLine("|---|------|--------------|-----------|-------------|---------|----------|----------|----------|");
    for (int i = 0; i < materials.Count; i++)
    {
        var m = materials[i];
        string nameEsc = m.Name.Replace("|", "\\|").Replace("\r", "").Replace("\n", " ");
        Console.WriteLine(
            $"| {i} | {nameEsc} | ({m.DiffuseR:F3},{m.DiffuseG:F3},{m.DiffuseB:F3},{m.DiffuseA:F3}) | {FmtTex(m.TextureIndex, textures)} | {FmtTex(m.SphereTextureIndex, textures)} | {m.SphereMode} | {m.ToonMode} | {FmtToonRef(m, textures)} | {m.NumFaceVertices} |");
    }
    Console.WriteLine();
    Console.WriteLine("---");
    Console.WriteLine();
    Console.WriteLine("生成: `Tools/PmxMaterialDump`（頂点・面はスキップし、テクスチャ表とマテリアルブロックのみ解析）");
}
catch (Exception ex)
{
    Console.Error.WriteLine("Parse error: " + ex);
    return 2;
}

return 0;

static string FmtTex(int idx, List<string> textures)
{
    if (idx < 0) return "—";
    if (idx >= textures.Count) return $"**無効#{idx}**";
    return $"`{textures[idx]}`";
}

static string FmtToonRef(MatRow m, List<string> textures)
{
    if (m.ToonMode == 0)
        return FmtTex(m.ToonTextureIndex, textures);
    return $"共有 **toon{m.ToonTextureIndex}**";
}

static PmxHeader ReadHeader(BinaryReader br)
{
    var sig = Encoding.ASCII.GetString(br.ReadBytes(4));
    if (sig != "PMX ")
        throw new InvalidDataException("Not PMX (expected 'PMX ')");
    return new PmxHeader
    {
        Version = br.ReadSingle(),
        DataSize = br.ReadByte(),
        Encode = br.ReadByte(),
        AddUVNum = br.ReadByte(),
        VertexIndexSize = br.ReadByte(),
        TextureIndexSize = br.ReadByte(),
        MaterialIndexSize = br.ReadByte(),
        BoneIndexSize = br.ReadByte(),
        MorphIndexSize = br.ReadByte(),
        RigidbodyIndexSize = br.ReadByte()
    };
}

static string ReadText(BinaryReader br, PmxHeader hdr)
{
    int n = br.ReadInt32();
    if (n <= 0) return "";
    var bytes = br.ReadBytes(n);
    if (hdr.Encode == 0)
        return Encoding.Unicode.GetString(bytes);
    return Encoding.UTF8.GetString(bytes);
}

static void ReadInfo(BinaryReader br, PmxHeader hdr)
{
    ReadText(br, hdr);
    ReadText(br, hdr);
    ReadText(br, hdr);
    ReadText(br, hdr);
}

static int ReadIndex(BinaryReader br, byte size)
{
    switch (size)
    {
        case 1:
            {
                byte x = br.ReadByte();
                return x == 0xFF ? -1 : x;
            }
        case 2:
            {
                ushort x = br.ReadUInt16();
                return x == 0xFFFF ? -1 : x;
            }
        case 4:
            return (int)br.ReadUInt32();
        default:
            throw new InvalidDataException("Bad index size: " + size);
    }
}

static void SkipIndex(BinaryReader br, byte sz) => _ = ReadIndex(br, sz);

static void SkipVertices(BinaryReader br, PmxHeader hdr)
{
    int count = br.ReadInt32();
    for (int i = 0; i < count; i++)
    {
        br.ReadBytes(12 + 12 + 8); // pos, normal, uv
        br.ReadBytes(16 * hdr.AddUVNum); // addUV: float4 each
        byte wtype = br.ReadByte();
        switch (wtype)
        {
            case 0: // BDEF1
                SkipIndex(br, hdr.BoneIndexSize);
                break;
            case 1: // BDEF2
                SkipIndex(br, hdr.BoneIndexSize);
                SkipIndex(br, hdr.BoneIndexSize);
                br.ReadBytes(4); // weight
                break;
            case 2: // BDEF4
                for (int k = 0; k < 4; k++) SkipIndex(br, hdr.BoneIndexSize);
                br.ReadBytes(16);
                break;
            case 3: // SDEF
                SkipIndex(br, hdr.BoneIndexSize);
                SkipIndex(br, hdr.BoneIndexSize);
                br.ReadBytes(4 + 12 + 12 + 12); // w, C, R0, R1
                break;
            case 4: // QDEF
                for (int k = 0; k < 4; k++) SkipIndex(br, hdr.BoneIndexSize);
                br.ReadBytes(16);
                break;
            default:
                throw new InvalidDataException("Unknown weight type: " + wtype);
        }
        br.ReadBytes(4); // edge scale (float)
    }
}

static void SkipFaces(BinaryReader br, PmxHeader hdr)
{
    int vertexIndexCount = br.ReadInt32();
    br.ReadBytes(vertexIndexCount * hdr.VertexIndexSize);
}

static List<string> ReadTextures(BinaryReader br, PmxHeader hdr)
{
    int n = br.ReadInt32();
    var list = new List<string>(n);
    for (int i = 0; i < n; i++)
        list.Add(ReadText(br, hdr));
    return list;
}

static List<MatRow> ReadMaterials(BinaryReader br, PmxHeader hdr)
{
    int n = br.ReadInt32();
    var list = new List<MatRow>(n);
    for (int i = 0; i < n; i++)
    {
        var name = ReadText(br, hdr);
        _ = ReadText(br, hdr);
        float dr = br.ReadSingle(), dg = br.ReadSingle(), db = br.ReadSingle(), da = br.ReadSingle();
        br.ReadBytes(12 + 4 + 12); // spec rgb, power, ambient rgb
        br.ReadByte(); // draw flags
        br.ReadBytes(16 + 4); // edge color vec4, edge size
        int tex = ReadIndex(br, hdr.TextureIndexSize);
        int sph = ReadIndex(br, hdr.TextureIndexSize);
        byte sphMode = br.ReadByte();
        byte toonMode = br.ReadByte();
        int toonIdx;
        if (toonMode == 0)
            toonIdx = ReadIndex(br, hdr.TextureIndexSize);
        else
            toonIdx = br.ReadByte();
        _ = ReadText(br, hdr);
        int faceVerts = br.ReadInt32();
        list.Add(new MatRow(name, dr, dg, db, da, tex, sph, sphMode, toonMode, toonIdx, faceVerts));
    }
    return list;
}

sealed class PmxHeader
{
    public float Version;
    public byte DataSize;
    public byte Encode;
    public byte AddUVNum;
    public byte VertexIndexSize;
    public byte TextureIndexSize;
    public byte MaterialIndexSize;
    public byte BoneIndexSize;
    public byte MorphIndexSize;
    public byte RigidbodyIndexSize;
}

sealed record MatRow(
    string Name,
    float DiffuseR, float DiffuseG, float DiffuseB, float DiffuseA,
    int TextureIndex,
    int SphereTextureIndex,
    byte SphereMode,
    byte ToonMode,
    int ToonTextureIndex,
    int NumFaceVertices);
