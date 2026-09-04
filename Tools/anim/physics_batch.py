import bpy, sys, math, os, struct
import numpy as np
from mathutils import Matrix, Vector
SCR=r"C:\Users\siuts\AppData\Local\Temp\claude\C--Users-siuts-source-repos-DX12\ad50df66-8bf7-4afe-906f-f92d9e3a13d8\scratchpad"
ANIMROOT=r"C:\Users\siuts\source\repos\DX12\assets\Animations"
OUTDIR=r"C:\Users\siuts\source\repos\DX12\assets\Animations\clips"
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
os.makedirs(OUTDIR, exist_ok=True)
argv=sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else []
FILTER=set(argv) if argv and argv[0]!="all" else None
def log(*a): print("[PB]",*a)
C=np.load(SCR+r"\bridge_C.npy"); Cinv=np.linalg.inv(C); Clin=C[:3,:3]
eng_names=set()
for ln in open(SCR+r"\engine_bones.txt",encoding="utf-8"):
    ln=ln.rstrip("\n")
    if ln and not ln.startswith("#"): eng_names.add(ln.split("\t",1)[0])
def to_assimp(bn):
    if bn.endswith(".R"): return "右"+bn[:-2]
    if bn.endswith(".L"): return "左"+bn[:-2]
    return bn

# (name, rel_fbx, loop?)
CLIPS=[
 ("idle","Characters/Mannequins/Anims/Unarmed/MM_Idle.fbx",True),
 ("walk","Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Fwd.fbx",True),
 ("run","Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.fbx",True),
 ("walk_bwd","Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Bwd.fbx",True),
 ("walk_left","Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Left.fbx",True),
 ("walk_right","Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Right.fbx",True),
 ("run_bwd","Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Bwd.fbx",True),
 ("attack1","Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.fbx",False),
 ("attack2","Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_02.fbx",False),
 ("attack3","Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_03.fbx",False),
 ("charged_attack","Characters/Mannequins/Anims/Unarmed/Attack/MM_ChargedAttack.fbx",False),
 ("death","Characters/Mannequins/Anims/Death/MM_Death_Front_01.fbx",False),
 ("hitreact","Characters/Mannequins/Anims/Rifle/HitReact/MM_HitReact_Front_Med_01.fbx",False),
 ("jump","Characters/Mannequins/Anims/Unarmed/Jump/MM_Jump.fbx",False),
 ("fall","Characters/Mannequins/Anims/Unarmed/Jump/MM_Fall_Loop.fbx",True),
 ("land","Characters/Mannequins/Anims/Unarmed/Jump/MM_Land.fbx",False),
 ("dash","Characters/Mannequins/Anims/Unarmed/Jump/MM_Dash.fbx",False),
 ("ka_idle_look","KawaiiAnimations/Animations/Idle/Anim_KA_Idle02_LookLeftAndRight.fbx",True),
 ("ka_combo1","KawaiiAnimations/Animations/Combat/Anim_KA_Combat_BareHands_Combo1.fbx",False),
 ("ka_sit","KawaiiAnimations/Animations/Sit/Anim_KA_Sit_CrossLegged_Loop.fbx",True),
 ("ka_sleep","KawaiiAnimations/Animations/Sleep/Anim_KA_Sleep_BendOneKnee_Loop.fbx",True),
 ("ka_dash","KawaiiAnimations/Animations/Action/Anim_KA_Dash_Fwd.fbx",False),
]
PREROLL=25

for mod in ("bl_ext.blender_org.mmd_tools","mmd_tools"):
    try: bpy.ops.preferences.addon_enable(module=mod); break
    except: pass
for o in list(bpy.data.objects): bpy.data.objects.remove(o,do_unlink=True)
bpy.ops.mmd_tools.import_model(filepath=PMX,scale=0.08,types={'MESH','ARMATURE','PHYSICS'},clean_model=False,log_level='ERROR')
hib=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]; hib.name="HIBANA"
meshes=[o for o in bpy.data.objects if o.type=='MESH']
root_empty=hib; p=hib.parent
while p: root_empty=p; p=p.parent
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
log("hibana+physics ready. rigidbodies=",sum(1 for o in bpy.data.objects if getattr(o,'rigid_body',None)),"export bones=",len(exp))
rw=bpy.context.scene.rigidbody_world
if rw:
    try: rw.substeps_per_frame=max(rw.substeps_per_frame,10); rw.solver_iterations=max(rw.solver_iterations,10)
    except: pass

def clear_anim():
    if hib.animation_data and hib.animation_data.action:
        act=hib.animation_data.action; bpy.data.actions.remove(act)

def export_one(name, fbxrel, loop):
    path=os.path.join(ANIMROOT, fbxrel.replace("/",os.sep))
    if not os.path.exists(path): log("MISS",name); return
    before=set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=path)
    new=[o for o in bpy.data.objects if o not in before]
    uel=[o for o in new if o.type=='ARMATURE']
    if not uel:
        for o in new: bpy.data.objects.remove(o,do_unlink=True);
        log("NOARM",name); return
    ue=uel[0]
    for o in new:
        if o.type=='MESH': bpy.data.objects.remove(o,do_unlink=True)
    bpy.context.view_layer.update()
    act=ue.animation_data.action; f0,f1=int(act.frame_range[0]),int(act.frame_range[1]); F=f1-f0+1
    Uw=ue.matrix_world
    pairs=[(s,t) for s,t in PAIRS if s in ue.pose.bones and t in hib.pose.bones]
    for _,t in pairs: hib.pose.bones[t].rotation_mode='QUATERNION'
    S_rest_wq={s:(Uw@ue.data.bones[s].matrix_local).to_quaternion() for s,t in pairs}
    T_rest_wq={t:(Hw@hib.data.bones[t].matrix_local).to_quaternion() for s,t in pairs}
    order=sorted(pairs,key=lambda st:depth(st[1]))
    sc=bpy.context.scene
    # pass1: basis quats + root track (from UE pelvis)
    qstore=[]; root=np.zeros((F,3),dtype=np.float32); pel0=None
    for fi in range(F):
        sc.frame_set(f0+fi)
        for s,t in order:
            upb=ue.pose.bones[s]
            dq=(Uw@upb.matrix).to_quaternion()@S_rest_wq[s].inverted()
            tq_arm=(Hw_inv.to_quaternion()@(dq@T_rest_wq[t]))
            tpb=hib.pose.bones[t]; cur=tpb.matrix.copy()
            tpb.matrix=Matrix.Translation(cur.translation)@tq_arm.to_matrix().to_4x4()
            bpy.context.view_layer.update()
        qstore.append({t:hib.pose.bones[t].rotation_quaternion.copy() for _,t in pairs})
        pelw=np.array((Uw@ue.pose.bones['pelvis'].matrix).translation)
        if pel0 is None: pel0=pelw
        root[fi]=(Clin@(pelw-pel0)).astype(np.float32)
    clear_anim()
    # write keyframes
    if loop:
        LOOPS=2 if F>120 else 3
        for cyc in range(LOOPS):
            for fi in range(F):
                fr=1+cyc*F+fi
                for _,t in pairs:
                    pb=hib.pose.bones[t]; pb.rotation_quaternion=qstore[fi][t]; pb.keyframe_insert('rotation_quaternion',frame=fr)
        TOT=LOOPS*F; win0=1+(LOOPS-1)*F
    else:
        # pre-roll hold frame0, then play once
        for fr in range(1,PREROLL+1):
            for _,t in pairs:
                pb=hib.pose.bones[t]; pb.rotation_quaternion=qstore[0][t]; pb.keyframe_insert('rotation_quaternion',frame=fr)
        for fi in range(F):
            fr=PREROLL+1+fi
            for _,t in pairs:
                pb=hib.pose.bones[t]; pb.rotation_quaternion=qstore[fi][t]; pb.keyframe_insert('rotation_quaternion',frame=fr)
        TOT=PREROLL+F; win0=PREROLL+1
    sc.frame_start=1; sc.frame_end=TOT
    if rw: rw.point_cache.frame_start=1; rw.point_cache.frame_end=TOT
    try: bpy.ops.mmd_tools.ptcache_rigid_body_delete_bake()
    except: pass
    bpy.context.view_layer.objects.active=root_empty
    try: bpy.ops.mmd_tools.ptcache_rigid_body_bake()
    except Exception as e: log("bake ERR",name,repr(e)[:120])
    # export window (F frames from win0), read ALL bones (incl physics)
    mats=np.zeros((F,len(exp),4,4),dtype=np.float32)
    for fi in range(F):
        sc.frame_set(win0+fi); bpy.context.view_layer.update()
        for bi,(an,bn,restinv) in enumerate(exp):
            pm=np.array(hib.pose.bones[bn].matrix)
            mats[fi,bi]=(C@(pm@restinv)@Cinv).astype(np.float32)
    dur=max(F/30.0,1e-3); horiz=np.sqrt(root[:,0]**2+root[:,2]**2); fwd=float(horiz[-1]/dur)
    out=os.path.join(OUTDIR,name+".skcl")
    with open(out,"wb") as fo:
        fo.write(b"SKCL"); fo.write(struct.pack("<I",2)); fo.write(struct.pack("<III",len(exp),F,30)); fo.write(struct.pack("<f",fwd))
        for an,bn,_ in exp:
            nb=an.encode("utf-8"); fo.write(struct.pack("<H",len(nb))); fo.write(nb)
        fo.write(mats.tobytes()); fo.write(root.tobytes())
    log("OK %-14s F=%3d loop=%d fwd=%.1f"%(name,F,int(loop),fwd))
    clear_anim(); bpy.data.objects.remove(ue,do_unlink=True)
    if act:
        try: bpy.data.actions.remove(act)
        except: pass

n=0
for nm,rel,loop in CLIPS:
    if FILTER and nm not in FILTER: continue
    export_one(nm,rel,loop); n+=1
log("DONE physics-baked",n,"clips")
