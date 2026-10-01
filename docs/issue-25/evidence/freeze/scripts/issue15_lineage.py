"""Check that each detected layout source has one content owner on a page."""
from collections import defaultdict


def lineage_failures(page):
    regions = {region['id']: region for region in page['regions']}
    layout_ids = {block['id'] for block in page['layout_blocks']}
    source_owners = defaultdict(set)
    failures = []
    for block in page['blocks']:
        for region_id in block['source_region_ids']:
            region = regions.get(region_id)
            if region is None:
                continue  # The caller reports the missing region separately.
            for source_id in region['source_layout_block_ids']:
                if source_id in layout_ids:
                    source_owners[source_id].add(block['id'])
    for source_id, owners in source_owners.items():
        if len(owners) != 1:
            failures.append(f'{source_id}:duplicate_content_lineage:{sorted(owners)}')
    relation_owner = {}
    for relation in page['relations']:
        if relation['type'] != 'content_owned_by':
            continue
        source_id = relation['source_layout_block_id']
        owner_id = relation['owner_block_id']
        if source_id in relation_owner:
            failures.append(f'{source_id}:duplicate_ownership_relation')
        relation_owner[source_id] = owner_id
        if source_owners.get(source_id) != {owner_id}:
            failures.append(f'{source_id}:owner_lineage_mismatch')
    return failures
