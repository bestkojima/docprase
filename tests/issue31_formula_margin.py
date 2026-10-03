"""公共 CLI：公式去边保留原图、真实输入坐标和安全重导出。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image, ImageDraw
import jsonschema
from printed_page_integration import ROOT, config


def main():
    fixture, production = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='issue31-formula-margin-') as directory:
        root = Path(directory)
        source = root / 'source.png'
        image = Image.new('RGB', (200, 160), 'white')
        ImageDraw.Draw(image).rectangle((50, 45, 149, 104), fill='black')
        image.save(source)
        setting = config('printed_page_structure')
        setting['execution'].update(layout_preprocess='reference', layout_score_threshold=.3,
                                    formula_crop_margin=True)
        cfg = root / 'config.json'; cfg.write_text(json.dumps(setting))
        trace = root / 'trace.json'
        trace.write_text(json.dumps(dict(candidates=[[5,.95,20,20,180,140,0]],
            outputs=[dict(bbox=[50,45,150,105],text='$2^{64}-1$'),
                     dict(bbox=[20,20,180,140],text='$2^{54}-1$')])))
        job = root / 'job'
        result = subprocess.run([fixture,'--config',str(cfg),'--input',str(source),'--out',str(job)],
            cwd=ROOT,env=dict(os.environ,DOCOCR_TEST_STRUCTURE_PATH=str(trace)),capture_output=True,text=True)
        assert result.returncode == 0, result.stderr
        document=json.loads((job/'document.json').read_text())
        assert document['schema_version']=='1.13'
        b=document['pages'][0]['blocks'][0]
        assert b['bbox']==[20,20,180,140] and b['content']['text']=='2^{64}-1'
        assert b['provenance']['recognition']['input_crop_bbox']==[50,45,150,105]
        visual=b['provenance']['visual']
        assert visual['input_crop_bbox']==[50,45,150,105] and visual['source_bbox']==b['bbox']
        assert Image.open(job/b['content']['resource']).size==(160,120)
        sx=100/visual['content_size'][0]; sy=60/visual['content_size'][1]
        assert visual['canvas_to_page_affine']==[sx,0,50-visual['pad_offset'][0]*sx,0,sy,45-visual['pad_offset'][1]*sy]
        schema=json.loads((ROOT/'schemas/document-ir/document-ir-1.13-image.schema.json').read_text())
        jsonschema.validate(document,schema)
        reexport=root/'reexport'
        result=subprocess.run([production,'--reexport',str(job/'document.json'),'--asset-root',str(job),
            '--out',str(reexport)],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        assert (job/'document.json').read_bytes()==(reexport/'document.json').read_bytes()
        assert (job/'document.md').read_bytes()==(reexport/'document.md').read_bytes()
        for invalid in ([0,0,150,105],[50,45,151,105]):
            altered=json.loads((job/'document.json').read_text())
            altered['pages'][0]['blocks'][0]['provenance']['visual']['input_crop_bbox']=invalid
            target=root/('bad-'+str(invalid[2])+'.json');target.write_text(json.dumps(altered))
            result=subprocess.run([production,'--reexport',str(target),'--asset-root',str(job),
                '--out',str(root/('bad-export-'+str(invalid[2])))],capture_output=True,text=True)
            assert result.returncode!=0,'篡改实际输入坐标不应被重新导出接受'
        for name, enabled, category, fill in [('disabled',False,5,None),
                                             ('uniform',True,5,'white'),
                                             ('text-task',True,22,None),
                                             ('tiny-content',True,5,'tiny')]:
            setting['execution']['formula_crop_margin']=enabled
            cfg.write_text(json.dumps(setting))
            if fill:
                candidate_image=Image.new('RGB',(200,160),'white')
                if fill=='tiny': ImageDraw.Draw(candidate_image).rectangle((50,45,51,46),fill='black')
                candidate_image.save(source)
            else: image.save(source)
            trace.write_text(json.dumps(dict(candidates=[[category,.95,20,20,180,140,0]],
                outputs=[dict(bbox=[20,20,180,140],text='$2^{54}-1$')])))
            output=root/name
            result=subprocess.run([fixture,'--config',str(cfg),'--input',str(source),'--out',str(output)],
                cwd=ROOT,env=dict(os.environ,DOCOCR_TEST_STRUCTURE_PATH=str(trace)),capture_output=True,text=True)
            assert result.returncode==0,result.stderr
            unchanged=json.loads((output/'document.json').read_text())
            assert unchanged['schema_version']=='1.10'
            assert 'input_crop_bbox' not in unchanged['pages'][0]['blocks'][0]['provenance']['recognition']
        print('formula margin: PASS')


if __name__=='__main__':
    main()
