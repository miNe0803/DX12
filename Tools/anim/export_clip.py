import bpy, sys, math, os, struct
import numpy as np
from mathutils import Matrix, Vector
argv=sys.argv[sys.argv.index("--")+1:]
ANIM=argv[0]; OUTCLIP=argv[1]; ZROT=math.radians(float(argv[2])) if len(argv)>2 else 0.0
SCR=r"C:\Users\siuts\AppData\Local\Temp\claude\C--Users-siuts-source-repos-DX12\ad50df66-8bf7-4afe-906f-f92d9e3a13d8\scratchpad"
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
def log(*a): print("[EXP]",*a)
C=np.load(SCR+r"\bridge_C.npy"); Cinv=np.linalg.inv(C)
# engine valid bone names
eng_names=set()
for ln in open(SCR+r"\engine_bones.txt",encoding="utf-8"):
    ln=ln.rstrip("\n")
    if ln and not ln.startswith("#"): eng_names.add(ln.split("\t",1)[0])
def to_assimp(bn):
    if bn.endswith(".R"): return "右"+bn[:-2]
    if bn.endswith(".L"): return "左"+bn[:-2]
    return bn

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
before=set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=ANIM)
new=[o for o in bpy.data.objects if o not in before]
ue=[o for o in new if o.type=='ARMATURE'][0]
for o in new:
    if o.type=='MESH': bpy.data.objects.remove(o,do_unlink=True)
ue.rotation_euler=(ue.rotation_euler[0],ue.rotation_euler[1],ue.rotation_euler[2]+ZROT); bpy.context.view_layer.update()
act=ue.animation_data.action; f0,f1=int(act.frame_range[0]),int(act.frame_range[1])
fps=act.frame_range  # not reliable; use 30
FPS=30.0
for pb in hib.pose.bones:
    for c in pb.constraints:
        if c.type=='IK': c.mute=True
# retarget map (control bones)
PAIRS=[('pelvis','下半身'),('spine_01','上半身'),('spine_02','上半身1'),('spine_03','上半身2'),
 ('neck_01','首'),('head','頭'),
 ('clavicle_l','肩.L'),('upperarm_l','腕.L'),('lowerarm_l','ひじ.L'),('hand_l','手首.L'),
 ('clavicle_r','肩.R'),('upperarm_r','腕.R'),('lowerarm_r','ひじ.R'),('hand_r','手首.R'),
 ('thigh_l','足.L'),('calf_l','ひざ.L'),('foot_l','足首.L'),('ball_l','つま先.L'),
 ('thigh_r','足.R'),('calf_r','ひざ.R'),('foot_r','足首.R'),('ball_r','つま先.R')]
pairs=[(s,t) for s,t in PAIRS if s in ue.pose.bones and t in hib.pose.bones]
for _,t in pairs: hib.pose.bones[t].rotation_mode='QUATERNION'
Uw=ue.matrix_world; Hw=hib.matrix_world; Hw_inv=Hw.inverted()
S_rest_wq={s:(Uw@ue.data.bones[s].matrix_local).to_quaternion() for s,t in pairs}
T_rest_wq={t:(Hw@hib.data.bones[t].matrix_local).to_quaternion() for s,t in pairs}
def depth(bn):
    d=0;b=hib.data.bones[bn]
    while b.parent:d+=1;b=b.parent
    return d
pairs.sort(key=lambda st:depth(st[1]))

# export bone set: weighted blender bones whose assimp name exists in engine
exp=[]   # (engine_name, blender_bone_name, rest_local_np_inv)
for b in hib.data.bones:
    if b.name in weighted:
        an=to_assimp(b.name)
        if an in eng_names:
            exp.append((an, b.name, np.array(b.matrix_local.inverted())))
log("export bones:",len(exp),"of weighted",len(weighted))

sc=bpy.context.scene
frames=list(range(f0,f1+1)); F=len(frames)
# data buffer
mats=np.zeros((F,len(exp),4,4),dtype=np.float32)
for fi,f in enumerate(frames):
    sc.frame_set(f)
    for s,t in pairs:
        upb=ue.pose.bones[s]
        dq=(Uw@upb.matrix).to_quaternion()@S_rest_wq[s].inverted()
        tq_arm=(Hw_inv.to_quaternion()@(dq@T_rest_wq[t]))
        tpb=hib.pose.bones[t]; cur=tpb.matrix.copy()
        tpb.matrix=Matrix.Translation(cur.translation)@tq_arm.to_matrix().to_4x4()
        bpy.context.view_layer.update()
    for bi,(an,bn,restinv) in enumerate(exp):
        pm=np.array(hib.pose.bones[bn].matrix)      # armature-space posed (col-vec)
        skinB=pm@restinv                            # model bind->posed (blender space)
        skinE=C@skinB@Cinv                          # -> engine model space
        mats[fi,bi]=skinE.astype(np.float32)
log("baked+converted frames",F)

# write clip file: magic, ver, N, F, fps, then N names, then F*N*16 floats (row-major)
with open(OUTCLIP,"wb") as fo:
    fo.write(b"SKCL"); fo.write(struct.pack("<I",1))
    fo.write(struct.pack("<III",len(exp),F,int(FPS)))
    for an,bn,_ in exp:
        nb=an.encode("utf-8"); fo.write(struct.pack("<H",len(nb))); fo.write(nb)
    fo.write(mats.tobytes())
log("WROTE",OUTCLIP,"size",os.path.getsize(OUTCLIP),"N",len(exp),"F",F)
