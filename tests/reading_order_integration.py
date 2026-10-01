"""通过 CLI 作业输出验收双栏阅读顺序、模型依据与图文关系。"""
from pathlib import Path
import sys
import tempfile

from PIL import Image
from printed_page_integration import run


def check(binary, root, image, scenario, source, reason):
    output, document, manifest = run(binary, scenario, root, image)
    assert document['schema_version'] == '1.6'
    page = document['pages'][0]
    blocks = page['blocks']
    candidate_by_layout = {b['id']: b['candidate_id'] for b in page['layout_blocks']
                           if 'candidate_id' in b}
    candidate_of = lambda block: candidate_by_layout[block['source_region_ids'][0].replace('r', 'l')]
    assert [candidate_of(b) for b in blocks] == [0, 1, 2, 3, 5, 6, 9, 4, 7, 8, 10]
    assert page['reading_order'] == [b['id'] for b in blocks]
    assert page['reading_order_evidence']['source'] == source
    assert page['reading_order_evidence']['reason'] == reason
    assert all(b['reading_order_source'] == source for b in blocks)
    assert [b['id'] for b in blocks] == [
        'b0001', 'b0002', 'b0003', 'b0004', 'b0006', 'b0008',
        'b0011', 'b0005', 'b0007', 'b0009', 'b0010']
    assert all(b['provenance']['request_id'] == 'req' + b['source_region_ids'][0]
               for b in blocks)
    assert {(r['type'], r['source_block_id'], r['target_block_id']) for r in page['relations']} == {
        ('caption_of', 'b0008', 'b0006'),
        ('caption_of', 'b0009', 'b0007'),
        ('footnote_of', 'b0011', 'b0004'),
        ('heading_precedes', 'b0003', 'b0004'),
        ('heading_precedes', 'b0003', 'b0005'),
    }
    assert document['layout_diagnostics']['candidates'][11]['filter_reason'] == 'below_score_threshold'
    assert next(b for b in blocks if candidate_of(b) == 10)['type'] == 'unknown'
    assert len(manifest['regions']) == 11
    markdown = (output / 'document.md').read_text()
    assert markdown.index('第一节') < markdown.index('左段¹') < markdown.index('图1 插图')
    assert markdown.index('图1 插图') < markdown.rindex('右段') < markdown.index('表1 统计')
    image_block = next(b for b in blocks if candidate_of(b) == 5)
    assert (output / image_block['content']['resource']).read_bytes().startswith(b'\x89PNG')


def main():
    with tempfile.TemporaryDirectory(prefix='dococr-order-') as temporary:
        root = Path(temporary)
        image = root / 'page.png'
        Image.new('RGB', (100, 100), 'white').save(image)
        check(sys.argv[1], root, image, 'printed_page_reading', 'geometry', 'model_column_conflict')
        check(sys.argv[1], root, image, 'printed_page_reading_rank', 'model', 'unique_rank')
        check(sys.argv[1], root, image, 'printed_page_reading_duplicate', 'geometry', 'duplicate_rank')
        check(sys.argv[1], root, image, 'printed_page_reading_missing', 'geometry', 'missing_rank')
        for scenario in ('printed_page_reading_short_title', 'printed_page_reading_tiny_title'):
            _, short_title, _ = run(sys.argv[1], scenario, root, image)
            short_page = short_title['pages'][0]
            short_map = {b['id']: b['candidate_id'] for b in short_page['layout_blocks']
                         if 'candidate_id' in b}
            assert [short_map[b['source_region_ids'][0].replace('r', 'l')]
                    for b in short_page['blocks']] == [0, 1, 2, 3, 5, 6, 9, 4, 7, 8, 10]
            if scenario.endswith('short_title'):
                assert {(r['source_block_id'], r['target_block_id']) for r in short_page['relations']
                        if r['type'] == 'heading_precedes'} == {('b0003', 'b0004'), ('b0003', 'b0005')}
            else:
                assert not any(r['type'] == 'heading_precedes' for r in short_page['relations'])
        _, columns, _ = run(sys.argv[1], 'printed_page_reading_columns', root, image)
        page = columns['pages'][0]
        by_layout = {b['id']: b['candidate_id'] for b in page['layout_blocks'] if 'candidate_id' in b}
        assert [by_layout[b['source_region_ids'][0].replace('r', 'l')] for b in page['blocks']] == [
            0, 2, 3, 5, 6, 9, 1, 4, 7, 8, 10]
        _, sectioned, _ = run(sys.argv[1], 'printed_page_reading_sectioned', root, image)
        page = sectioned['pages'][0]
        by_layout = {b['id']: b['candidate_id'] for b in page['layout_blocks']
                     if 'candidate_id' in b}
        assert [by_layout[b['source_region_ids'][0].replace('r', 'l')] for b in page['blocks']] == [
            0, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 11]
        assert page['reading_order'] == [b['id'] for b in page['blocks']]
        assert page['reading_order_evidence'] == {'source': 'geometry', 'reason': 'duplicate_rank'}
        _, local_title, _ = run(sys.argv[1], 'printed_page_reading_local_title', root, image)
        page = local_title['pages'][0]
        by_layout = {b['id']: b['candidate_id'] for b in page['layout_blocks']
                     if 'candidate_id' in b}
        assert [by_layout[b['source_region_ids'][0].replace('r', 'l')] for b in page['blocks']] == [
            0, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 11]
        _, single, _ = run(sys.argv[1], 'printed_page_reading_single', root, image)
        assert [b['id'] for b in single['pages'][0]['blocks']] == [
            'b0001', 'b0002', 'b0003', 'b0004']
        assert len([r for r in single['pages'][0]['relations'] if r['type'] == 'caption_of']) == 1
        _, model_order, _ = run(sys.argv[1], 'printed_page_reading_model_order', root, image)
        assert model_order['pages'][0]['reading_order'] == ['b0002', 'b0001', 'b0003']
        assert model_order['pages'][0]['reading_order_evidence'] == {
            'source': 'model', 'reason': 'unique_rank'}
        _, ambiguous, _ = run(sys.argv[1], 'printed_page_reading_ambiguous', root, image)
        assert any(r['type'] == 'caption_of' for r in ambiguous['pages'][0]['relations'])
        assert [c['filter_reason'] for c in ambiguous['layout_diagnostics']['candidates']
                if c['filter_reason'] == 'nms_same_class'] == ['nms_same_class'] * 2
        _, prose, _ = run(sys.argv[1], 'printed_page_reading_table_prose', root, image)
        assert len([r for r in prose['pages'][0]['relations'] if r['type'] == 'caption_of']) == 1
        for scenario in ('printed_page_reading_footnote_double',
                         'printed_page_reading_footnote_vision',
                         'printed_page_reading_footnote_composite'):
            _, unlinked, _ = run(sys.argv[1], scenario, root, image)
            assert not any(r['type'] == 'footnote_of' for r in unlinked['pages'][0]['relations'])
        _, footer, _ = run(sys.argv[1], 'printed_page_reading_footer', root, image)
        footer_page = footer['pages'][0]
        footer_candidates = {b['id']: b['candidate_id'] for b in footer_page['layout_blocks']
                             if 'candidate_id' in b}
        footer_order = [footer_candidates[b['source_region_ids'][0].replace('r', 'l')]
                        for b in footer_page['blocks']]
        assert footer_order.index(4) > footer_order.index(9)
        assert footer_order[-1] == 11
        _, owned, _ = run(sys.argv[1], 'printed_page_reading_owned', root, image)
        _, owned_rank, _ = run(sys.argv[1], 'printed_page_reading_owned_rank', root, image)
        for doc in (owned, owned_rank):
            page = doc['pages'][0]
            assert len([r for r in page['relations'] if r['type'] == 'content_owned_by']) == 2
            assert len(page['blocks']) == 10
            table = next(b for b in page['blocks'] if b['type'] == 'table')
            owner_relations = [r for r in page['relations'] if r['type'] == 'content_owned_by']
            assert all(r['owner_block_id'] == table['id'] for r in owner_relations)
            region = next(r for r in page['regions'] if r['id'] == table['source_region_ids'][0])
            assert all(r['source_layout_block_id'] in region['source_layout_block_ids']
                       for r in owner_relations)
        assert {r['source_layout_block_id']: r['owner_block_id'] for r in
                owned['pages'][0]['relations'] if r['type'] == 'content_owned_by'} == {
                r['source_layout_block_id']: r['owner_block_id'] for r in
                owned_rank['pages'][0]['relations'] if r['type'] == 'content_owned_by'}
        assert {b['id']: b['source_region_ids'] for b in owned['pages'][0]['blocks']} == {
            b['id']: b['source_region_ids'] for b in owned_rank['pages'][0]['blocks']}


if __name__ == '__main__':
    main()
