import json,subprocess,time,hashlib
from pathlib import Path
import cv2
import numpy as np
R=Path('/home/dr/project/docprase'); out=R/'output/issue31-margin-probe-20261003';out.mkdir()
source=R/'output/issue31-input-final-20261003/evaluation/candidate-07/job'
if not source.exists():
 source=next((R/'output/issue31-input-final-20261003').rglob('document.json')).parent
D=json.loads((source/'document.json').read_text());rows=[]
for block in D['pages'][0]['blocks']:
 if block['type']!='formula':continue
 image=source/block['content']['resource'] if block['content'].get('resource') else source/'assets'/f"p0001-{block['id']}.png"
 im=cv2.imread(str(image));gray=cv2.cvtColor(im,cv2.COLOR_BGR2GRAY);mn,mx=int(gray.min()),int(gray.max())
 lut=np.array([int((v-mn)/(mx-mn)*255) if mn<=v<=mx and mn!=mx else 0 for v in range(256)],dtype=np.uint8)
 mask=cv2.threshold(cv2.LUT(gray,lut),200,255,cv2.THRESH_BINARY_INV)[1]
 x,y,w,h=cv2.boundingRect(cv2.findNonZero(mask));folder=out/block['id'];folder.mkdir()
 crop=folder/'crop.png';cv2.imwrite(str(crop),im[y:y+h,x:x+w])
 cmd=[str(R/'output/resize-sweep-20261002/resize_sweep_probe'),str(R/'output/issue31-attribution-20261003/probe-config.json'),str(crop),str(R/'output/issue31-attribution-20261003/prompt.txt'),str(folder),'65536','313600','4096','1','0']
 t=time.monotonic()
 with (folder/'stdout.log').open('w') as so,(folder/'stderr.log').open('w') as se:result=subprocess.run(cmd,stdout=so,stderr=se,timeout=180)
 row=dict(block_id=block['id'],command=cmd,returncode=result.returncode,seconds=time.monotonic()-t,source=str(image),source_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),margin_box=[x,y,w,h],before=block['provenance']['raw_output'],after=(folder/'raw-0.txt').read_text())
 rows.append(row);(out/'report.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2));print(row['block_id'],row['before'],row['after'],flush=True)
