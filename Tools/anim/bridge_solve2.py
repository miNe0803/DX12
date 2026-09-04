import json, numpy as np
SCR=r"C:\Users\siuts\AppData\Local\Temp\claude\C--Users-siuts-source-repos-DX12\ad50df66-8bf7-4afe-906f-f92d9e3a13d8\scratchpad"
eng={}
for ln in open(SCR+r"\engine_bones.txt",encoding="utf-8"):
    ln=ln.rstrip("\n")
    if not ln or ln.startswith("#"): continue
    nm,rest=ln.split("\t",1)
    eng[nm]=np.array([float(x) for x in rest.split()],dtype=np.float64).reshape(4,4)
bj=json.load(open(SCR+r"\blender_bones.json",encoding="utf-8"))
def to_assimp(bn):
    if bn.endswith(".R"): return "右"+bn[:-2]
    if bn.endswith(".L"): return "左"+bn[:-2]
    return bn
# bone-origin correspondences (frame-independent physical joints)
Pb=[]; Pe=[]; used=[]
for r in bj["bones"]:
    an=to_assimp(r["blender"])
    if an not in eng: continue
    Ae=eng[an]
    Reng=np.linalg.inv(Ae)            # offset is column-vector (v'=Ae v); inv = bind rest (bone->model)
    pe=Reng[:3,3]
    Ab=np.array(r["rest"],dtype=np.float64)
    pb=Ab[:3,3]                        # blender armature-space bone head
    Pb.append([pb[0],pb[1],pb[2],1.0]); Pe.append(pe); used.append(r["blender"])
Pb=np.array(Pb); Pe=np.array(Pe)
# solve C3x4 :  Pe ~= Pb @ C3x4^T   ->  C3x4^T = lstsq(Pb, Pe)
CT,res,rank,sv=np.linalg.lstsq(Pb,Pe,rcond=None)   # CT: 4x3
C=np.eye(4); C[:3,:4]=CT.T
pred=Pb@CT
err=np.linalg.norm(pred-Pe,axis=1)
print("points:",len(Pb))
print("C=\n",np.array2string(C,precision=5,suppress_small=True))
print("residual(m): median=%.5f mean=%.5f max=%.5f"%(np.median(err),err.mean(),err.max()))
M=C[:3,:3]
print("det(C3x3)=%.5f (neg=handedness flip, expected LH vs RH)"%np.linalg.det(M))
u,s,vt=np.linalg.svd(M)
print("singular values (uniform scale check):",np.array2string(s,precision=5))
np.save(SCR+r"\bridge_C.npy",C)
print("saved bridge_C.npy")
