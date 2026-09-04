import bpy, sys, json
PMX=r"C:\Users\siuts\source\repos\DX12\assets\hibana\hibana.pmx"
OUT=sys.argv[-1]
for mod in ("bl_ext.blender_org.mmd_tools","mmd_tools"):
    try: bpy.ops.preferences.addon_enable(module=mod); break
    except: pass
for o in list(bpy.data.objects): bpy.data.objects.remove(o,do_unlink=True)
bpy.ops.mmd_tools.import_model(filepath=PMX,scale=0.08,types={'MESH','ARMATURE'},clean_model=False,log_level='ERROR')
arm=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]
meshes=[o for o in bpy.data.objects if o.type=='MESH']
# which bones are actually weighted (vertex groups across meshes)
weighted=set()
for m in meshes:
    for vg in m.vertex_groups: weighted.add(vg.name)
rows=[]
for b in arm.data.bones:
    mb=getattr(b,'mmd_bone',None)
    nj = mb.name_j if mb and getattr(mb,'name_j','') else ''
    ne = mb.name_e if mb and getattr(mb,'name_e','') else ''
    Ml = b.matrix_local  # rest, armature space (4x4)
    rows.append({
        "blender": b.name,
        "name_j": nj,
        "name_e": ne,
        "weighted": b.name in weighted,
        "parent": b.parent.name if b.parent else "",
        "rest": [list(Ml[r]) for r in range(4)],  # row-major 4x4
    })
data={"scale":0.08,"armature":arm.name,"count":len(rows),"weighted_count":len(weighted),"bones":rows}
open(OUT,"w",encoding="utf-8").write(json.dumps(data,ensure_ascii=False))
print("[DUMP] wrote",OUT,"bones",len(rows),"weighted",len(weighted))
# quick preview of weighted core bones name_j
core=[r for r in rows if r["weighted"] and any(k in r["name_j"] for k in ("上半身","下半身","腕","ひじ","手首","足","ひざ","足首","首","頭","肩","センター"))]
print("[DUMP] sample weighted name_j:", [r["name_j"] for r in core[:24]])
