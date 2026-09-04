import bpy
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
OUT=r"C:\Users\siuts\source\repos\DX12\assets\Animations\clips\springbones.rig"
SCR=r"C:\Users\siuts\AppData\Local\Temp\claude\C--Users-siuts-source-repos-DX12\ad50df66-8bf7-4afe-906f-f92d9e3a13d8\scratchpad"
def log(*a): print("[RIG]",*a)
# engine names set (only export bones the engine actually has, weighted)
eng_names=set()
for ln in open(SCR+r"\engine_bones.txt",encoding="utf-8"):
    ln=ln.rstrip("\n")
    if ln and not ln.startswith("#"): eng_names.add(ln.split("\t",1)[0])
for mod in ("bl_ext.blender_org.mmd_tools","mmd_tools"):
    try: bpy.ops.preferences.addon_enable(module=mod); break
    except: pass
for o in list(bpy.data.objects): bpy.data.objects.remove(o,do_unlink=True)
bpy.ops.mmd_tools.import_model(filepath=PMX,scale=0.08,types={'MESH','ARMATURE','PHYSICS'},clean_model=False,log_level='ERROR')
arm=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]
meshes=[o for o in bpy.data.objects if o.type=='MESH']
weighted=set()
for m in meshes:
    for vg in m.vertex_groups: weighted.add(vg.name)
# dynamic physics bones
phys=set()
for o in bpy.data.objects:
    rb=getattr(o,'rigid_body',None); mr=getattr(o,'mmd_rigid',None)
    if rb and mr and getattr(mr,'bone',''):
        try: t=int(mr.type)
        except: t=1
        if t in (1,2): phys.add(mr.bone)
def to_assimp(bn):
    if bn.endswith(".R"): return "右"+bn[:-2]
    if bn.endswith(".L"): return "左"+bn[:-2]
    return bn
def category(bn):
    for k,label in [('馬尾','longhair'),('側髪','sidehair'),('劉海','banghair'),('碎髮','strayhair'),('髮','hair'),('髪','hair'),
                    ('裙','skirt'),('袖','sleeve'),('後腰結','ribbon'),('尻尾','tail'),('耳环','earring'),
                    ('領','collar'),('中領','collar'),('颈饰','necklace'),('肩飾','shoulder'),('胸','chest'),
                    ('帽','hat'),('腰結','ribbon')]:
        if k in bn: return label
    return 'other'
# C scale for bone length -> engine model units
import numpy as np
C=np.load(SCR+r"\bridge_C.npy"); scaleC=float(np.linalg.norm(C[:3,0]))  # ~12.5
rows=[]
for bn in sorted(phys):
    if bn not in arm.data.bones: continue
    an=to_assimp(bn)
    if an not in eng_names or bn not in weighted: continue
    b=arm.data.bones[bn]
    par=b.parent.name if b.parent else ''
    pan=to_assimp(par) if par else ''
    blen=b.length*scaleC
    rows.append((an,pan,category(bn),blen))
# write: header + tab lines
with open(OUT,"w",encoding="utf-8") as f:
    f.write("# springbones v1  engine_name\tparent_engine_name\tcategory\tboneLen_engine\n")
    f.write("# count %d\n"%len(rows))
    for an,pan,cat,blen in rows:
        f.write("%s\t%s\t%s\t%.5f\n"%(an,pan,cat,blen))
from collections import Counter
log("wrote",OUT,"bones",len(rows))
log("by category:",dict(Counter(r[2] for r in rows)))
log("scaleC=",round(scaleC,3))
