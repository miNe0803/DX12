import bpy, sys, math, os
import numpy as np
from mathutils import Matrix, Vector
argv=sys.argv[sys.argv.index("--")+1:]
ANIM=argv[0]; OUTP=argv[1]; ZROT=math.radians(float(argv[2])) if len(argv)>2 else 0.0
NCOL=6
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
def log(*a): print("[S4]",*a)
for mod in ("bl_ext.blender_org.mmd_tools","mmd_tools"):
    try: bpy.ops.preferences.addon_enable(module=mod); break
    except: pass
for o in list(bpy.data.objects): bpy.data.objects.remove(o,do_unlink=True)
bpy.ops.mmd_tools.import_model(filepath=PMX,scale=0.08,types={'MESH','ARMATURE'},clean_model=False,log_level='ERROR')
hib=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]; hib.name="HIBANA"
meshes=[o for o in bpy.data.objects if o.type=='MESH']
before=set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=ANIM)
new=[o for o in bpy.data.objects if o not in before]
ue=[o for o in new if o.type=='ARMATURE'][0]
for o in new:
    if o.type=='MESH': bpy.data.objects.remove(o,do_unlink=True)
ue.rotation_euler=(ue.rotation_euler[0],ue.rotation_euler[1],ue.rotation_euler[2]+ZROT); bpy.context.view_layer.update()
act=ue.animation_data.action; f0,f1=int(act.frame_range[0]),int(act.frame_range[1])
log("clip",os.path.basename(ANIM),"frames",f0,f1)
for pb in hib.pose.bones:
    for c in pb.constraints:
        if c.type=='IK': c.mute=True
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
sc=bpy.context.scene
for f in range(f0,f1+1):
    sc.frame_set(f)
    for s,t in pairs:
        upb=ue.pose.bones[s]
        dq=(Uw@upb.matrix).to_quaternion()@S_rest_wq[s].inverted()
        tq_arm=(Hw_inv.to_quaternion()@(dq@T_rest_wq[t]))
        tpb=hib.pose.bones[t]; cur=tpb.matrix.copy()
        tpb.matrix=Matrix.Translation(cur.translation)@tq_arm.to_matrix().to_4x4()
        bpy.context.view_layer.update()
    for s,t in pairs: hib.pose.bones[t].keyframe_insert('rotation_quaternion',frame=f)
log("bake done")
# scene
mn=Vector((1e9,)*3);mx=Vector((-1e9,)*3);sc.frame_set(f0)
for m in meshes:
    for c in m.bound_box:
        w=m.matrix_world@Vector(c);mn=Vector((min(mn[i],w[i]) for i in range(3)));mx=Vector((max(mx[i],w[i]) for i in range(3)))
center=(mn+mx)*0.5;H=max((mx-mn).z,0.1)
cam_d=bpy.data.cameras.new("C");cam=bpy.data.objects.new("C",cam_d);sc.collection.objects.link(cam)
cam.location=(center.x+H*0.3,center.y-(H*1.9+0.6),center.z+H*0.0);cam.rotation_euler=(math.radians(90),0,math.radians(10));cam_d.lens=40
sc.camera=cam
sd=bpy.data.lights.new("S",'SUN');sd.energy=4.5;su=bpy.data.objects.new("S",sd);sc.collection.objects.link(su)
su.rotation_euler=(math.radians(55),math.radians(15),math.radians(40))
bpy.ops.mesh.primitive_plane_add(size=8,location=(center.x,center.y,0.0))
w=bpy.data.worlds.new("W");sc.world=w;w.use_nodes=True
w.node_tree.nodes["Background"].inputs[0].default_value=(0.32,0.35,0.40,1);w.node_tree.nodes["Background"].inputs[1].default_value=0.55
for eng in ("BLENDER_EEVEE_NEXT","BLENDER_EEVEE"):
    try:sc.render.engine=eng;break
    except:pass
RW,RH=300,440
sc.render.resolution_x=RW;sc.render.resolution_y=RH
try:sc.eevee.taa_render_samples=12
except:pass
sc.render.image_settings.file_format='PNG'
frames=[int(round(f0+(f1-f0)*k/(NCOL-1))) for k in range(NCOL)]
tiles=[]
for fr in frames:
    sc.frame_set(fr); p=f"{OUTP}_tmp_{fr}.png"; sc.render.filepath=p
    bpy.ops.render.render(write_still=True)
    img=bpy.data.images.load(p)
    a=np.array(img.pixels[:],dtype=np.float32).reshape(img.size[1],img.size[0],4)
    a=a[::-1]  # flip vertical (blender bottom-up)
    tiles.append(a); bpy.data.images.remove(img)
sheet=np.concatenate(tiles,axis=1)  # horizontal strip
Hs,Ws,_=sheet.shape
out=bpy.data.images.new("sheet",width=Ws,height=Hs)
out.pixels[:]=sheet[::-1].reshape(-1)  # back to bottom-up for save
out.filepath_raw=OUTP+"_sheet.png"; out.file_format='PNG'; out.save()
log("SHEET ->",OUTP+"_sheet.png","frames",frames,"size",Ws,Hs)
for fr in frames:
    try: os.remove(f"{OUTP}_tmp_{fr}.png")
    except: pass
