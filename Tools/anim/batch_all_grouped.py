import bpy, sys, math, os, struct
import numpy as np
from mathutils import Matrix, Vector
SCR=r"C:\Users\siuts\AppData\Local\Temp\claude\C--Users-siuts-source-repos-DX12\ad50df66-8bf7-4afe-906f-f92d9e3a13d8\scratchpad"
ANIMROOT=r"C:\Users\siuts\source\repos\DX12\assets\Animations"
GROUPROOT=os.path.join(ANIMROOT, "clips", "groups")
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
os.makedirs(GROUPROOT, exist_ok=True)
def log(*a): print("[ALL]",*a); sys.stdout.flush()
C=np.load(SCR+r"\bridge_C.npy"); Cinv=np.linalg.inv(C); Clin=C[:3,:3]
eng_names=set()
for ln in open(SCR+r"\engine_bones.txt",encoding="utf-8"):
    ln=ln.rstrip("\n")
    if ln and not ln.startswith("#"): eng_names.add(ln.split("\t",1)[0])
def to_assimp(bn):
    if bn.endswith(".R"): return "右"+bn[:-2]
    if bn.endswith(".L"): return "左"+bn[:-2]
    return bn

def group_of(relpath):
    # relpath uses os.sep, relative to ANIMROOT
    parts = relpath.replace("\\","/").split("/")
    parts = parts[:-1]  # drop filename
    low = [p.lower() for p in parts]
    if "demo" in low or "rigs" in low: return None       # デモ/リグは除外
    if "mannequins" in low and "anims" in low:
        i = low.index("anims")
        tail = parts[i+1:]
        return "MM_" + ("_".join(tail) if tail else "Root")
    if "kawaiianimations" in low and "animations" in low:
        i = low.index("animations")
        tail = parts[i+1:]
        return "KA_" + ("_".join(tail) if tail else "Root")
    return "Other_" + ("_".join(parts[-2:]) if len(parts)>=2 else "misc")

# enumerate all fbx (skip clips/)
all_fbx=[]
for root,dirs,files in os.walk(ANIMROOT):
    if os.path.join(ANIMROOT,"clips").lower() in root.lower(): continue
    for f in files:
        if f.lower().endswith(".fbx"):
            full=os.path.join(root,f); rel=os.path.relpath(full,ANIMROOT)
            g=group_of(rel)
            if g: all_fbx.append((g, os.path.splitext(f)[0], full))
log("total fbx:",len(all_fbx),"groups:",len(set(g for g,_,_ in all_fbx)))

# ---- hibana once ----
for mod in ("bl_ext.blender_org.mmd_tools","mmd_tools"):
    try: bpy.ops.preferences.addon_enable(module=mod); break
    except: pass
for o in list(bpy.data.objects): bpy.data.objects.remove(o,do_unlink=True)
bpy.ops.mmd_tools.import_model(filepath=PMX,scale=0.08,types={'MESH','ARMATURE'},clean_model=False,log_level='ERROR')
hib=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]; hib.name="HIBANA"
meshes=[o for o in bpy.data.objects if o.type=='MESH']
weighted=set()
for m in meshes:
    for vg in m.vertex_groups: weighted.add(vg.name)
for pb in hib.pose.bones:
    for c in pb.constraints:
        if c.type=='IK': c.mute=True
PAIRS=[('pelvis','下半身'),('spine_01','上半身'),('spine_02','上半身1'),('spine_03','上半身2'),
 ('neck_01','首'),('head','頭'),
 ('clavicle_l','肩.L'),('upperarm_l','腕.L'),('lowerarm_l','ひじ.L'),('hand_l','手首.L'),
 ('clavicle_r','肩.R'),('upperarm_r','腕.R'),('lowerarm_r','ひじ.R'),('hand_r','手首.R'),
 ('thigh_l','足.L'),('calf_l','ひざ.L'),('foot_l','足首.L'),('ball_l','つま先.L'),
 ('thigh_r','足.R'),('calf_r','ひざ.R'),('foot_r','足首.R'),('ball_r','つま先.R')]
Hw=hib.matrix_world; Hw_inv=Hw.inverted()
def depth(bn):
    d=0;b=hib.data.bones[bn]
    while b.parent:d+=1;b=b.parent
    return d
exp=[]
for b in hib.data.bones:
    if b.name in weighted:
        an=to_assimp(b.name)
        if an in eng_names: exp.append((an,b.name,np.array(b.matrix_local.inverted())))
log("export bones:",len(exp))

def convert(group, name, fbx):
    outdir=os.path.join(GROUPROOT, group); os.makedirs(outdir, exist_ok=True)
    out=os.path.join(outdir, name+".skcl")
    if os.path.exists(out): return "skip"
    before=set(bpy.data.objects)
    try: bpy.ops.import_scene.fbx(filepath=fbx)
    except Exception as e: return "importfail"
    new=[o for o in bpy.data.objects if o not in before]
    uel=[o for o in new if o.type=='ARMATURE']
    if not uel:
        for o in new: bpy.data.objects.remove(o,do_unlink=True)
        return "noarm"
    ue=uel[0]
    for o in new:
        if o.type=='MESH': bpy.data.objects.remove(o,do_unlink=True)
    bpy.context.view_layer.update()
    act=ue.animation_data.action if ue.animation_data else None
    if not act:
        bpy.data.objects.remove(ue,do_unlink=True); return "noaction"
    f0,f1=int(act.frame_range[0]),int(act.frame_range[1]); F=max(1,f1-f0+1)
    Uw=ue.matrix_world
    pairs=[(s,t) for s,t in PAIRS if s in ue.pose.bones and t in hib.pose.bones]
    for _,t in pairs: hib.pose.bones[t].rotation_mode='QUATERNION'
    S_rest_wq={s:(Uw@ue.data.bones[s].matrix_local).to_quaternion() for s,t in pairs}
    T_rest_wq={t:(Hw@hib.data.bones[t].matrix_local).to_quaternion() for s,t in pairs}
    order=sorted(pairs,key=lambda st:depth(st[1]))
    mats=np.zeros((F,len(exp),4,4),dtype=np.float32); root=np.zeros((F,3),dtype=np.float32); pel0=None
    sc=bpy.context.scene
    for fi in range(F):
        sc.frame_set(f0+fi)
        for s,t in order:
            upb=ue.pose.bones[s]
            dq=(Uw@upb.matrix).to_quaternion()@S_rest_wq[s].inverted()
            tq_arm=(Hw_inv.to_quaternion()@(dq@T_rest_wq[t]))
            tpb=hib.pose.bones[t]; cur=tpb.matrix.copy()
            tpb.matrix=Matrix.Translation(cur.translation)@tq_arm.to_matrix().to_4x4()
            bpy.context.view_layer.update()
        for bi,(an,bn,restinv) in enumerate(exp):
            pm=np.array(hib.pose.bones[bn].matrix)
            mats[fi,bi]=(C@(pm@restinv)@Cinv).astype(np.float32)
        pelw=np.array((Uw@ue.pose.bones['pelvis'].matrix).translation) if 'pelvis' in ue.pose.bones else np.zeros(3)
        if pel0 is None: pel0=pelw
        root[fi]=(Clin@(pelw-pel0)).astype(np.float32)
    dur=max(F/30.0,1e-3); horiz=np.sqrt(root[:,0]**2+root[:,2]**2); fwd=float(horiz[-1]/dur)
    with open(out,"wb") as fo:
        fo.write(b"SKCL"); fo.write(struct.pack("<I",2)); fo.write(struct.pack("<III",len(exp),F,30)); fo.write(struct.pack("<f",fwd))
        for an,bn,_ in exp:
            nb=an.encode("utf-8"); fo.write(struct.pack("<H",len(nb))); fo.write(nb)
        fo.write(mats.tobytes()); fo.write(root.tobytes())
    bpy.data.objects.remove(ue,do_unlink=True)
    try: bpy.data.actions.remove(act)
    except: pass
    return "ok"

done=0; skip=0; fail=0
for gi,(group,name,fbx) in enumerate(all_fbx):
    r=convert(group,name,fbx)
    if r=="ok": done+=1
    elif r=="skip": skip+=1
    else: fail+=1
    if (gi % 25)==0:
        try: bpy.ops.outliner.orphans_purge(do_recursive=True)
        except: pass
        log("progress %d/%d  ok=%d skip=%d fail=%d  [%s/%s]"%(gi+1,len(all_fbx),done,skip,fail,group,name))
log("DONE ok=%d skip=%d fail=%d total=%d"%(done,skip,fail,len(all_fbx)))
# group summary
from collections import Counter
cnt=Counter()
for g,_,_ in all_fbx: cnt[g]+=1
for g,n in sorted(cnt.items()): log("  group %-28s %d"%(g,n))
