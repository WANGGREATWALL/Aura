#!/usr/bin/env python3
"""Create an isolated fixed-ref comparison tree; never changes the working source."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--ref', default='e3437895a92c38dc75b676df1ec025c271600d33')
parser.add_argument('--queue', choices=['ring', 'mutex'], default='ring')
parser.add_argument('--records', choices=['pooled', 'system'], default='pooled')
parser.add_argument('--payload', choices=['pooled', 'small', 'system'], default='pooled')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
out = args.output.resolve()
if out.exists():
    parser.error('output must not exist; existing data is never overwritten')
commit = subprocess.check_output(['git', 'rev-parse', args.ref + '^{commit}'], cwd=repo, text=True).strip()
archive = subprocess.check_output(['git', 'archive', commit], cwd=repo)
out.mkdir(parents=True)
with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
    tar.extractall(out, filter='data')
for name in ['flow_reference_queue.h', 'flow_reference_allocators.h']:
    (out / 'src/flow' / name).write_bytes((repo / 'test' / name).read_bytes())
if args.queue == 'mutex':
    p = out / 'src/flow/ready_queue.h'
    text = p.read_text()
    needle = 'ReadyCounter, 256> ReadyQueue;'
    assert needle in text
    text = text.replace(needle, 'ReadyCounter, 256> RingReadyQueue;')
    end = text.rfind('#endif')
    text = text[:end] + '\n#include "flow/flow_reference_queue.h"\nnamespace au { namespace flow { namespace detail {\ntypedef MutexReadyQueue ReadyQueue;\n} } }\n' + text[end:]
    p.write_text(text)
if args.records == 'system':
    p = out / 'src/flow/core.h'
    text = p.read_text().replace('#include "flow/slab.h"', '#include "flow/slab.h"\n#include "flow/flow_reference_allocators.h"')
    for record, count in [('Node', 128), ('Edge', 256), ('ExternalToken', 64)]:
        needle = f'Slab<{record}, {count}>'
        assert needle in text
        text = text.replace(needle, f'SystemRecords<{record}, {count}>')
    p.write_text(text)
if args.payload == 'small':
    p = out / 'src/flow/payload_pool.h'
    text = p.read_text()
    for size, member in [(256, 'mMedium'), (1024, 'mLarge'), (4096, 'mExtra')]:
        text = text.replace(f'        if (size <= {size}) return {member}.create();\n', '')
        text = text.replace(f'        else if (size <= {size}) {member}.destroy(static_cast<au::flow::detail::PayloadBlock<{size}>*>(pointer));\n', '')
    text = '\n'.join(line for line in text.split('\n') if not any(('> '+member+';') in line for member in ['mMedium', 'mLarge', 'mExtra']))
    assert 'mMedium' not in text and 'mLarge' not in text and 'mExtra' not in text
    p.write_text(text)
if args.payload == 'system':
    (out / 'src/flow/payload_pool.h').write_text('''#ifndef AURA_FLOW_PAYLOAD_POOL_H_
#define AURA_FLOW_PAYLOAD_POOL_H_
#include "flow/flow_reference_allocators.h"
namespace au { namespace flow { namespace detail {
class PayloadPool final : public SystemPayloadAllocator {};
} } }
#endif
''')
# Build the same latest measurement tool against the fixed scheduler variant.
for name in ['flow_mechanism_benchmark.cpp', 'test_flow_mechanisms.cpp', 'flow_gpu_gap_benchmark.cpp', 'flow_diagnostics_benchmark.cpp', 'flow_payload_benchmark.cpp']:
    p = repo / 'test' / name
    (out / 'test' / name).write_bytes(p.read_bytes())
for name in ['flow_reference_queue.h', 'flow_reference_allocators.h']:
    (out / 'test' / name).write_bytes((repo / 'test' / name).read_bytes())
p = out / 'CMakeLists.txt'
text = p.read_text()
text += '''
if(ENABLE_FLOW_BENCHMARKS)
    add_executable(flow_mechanism_benchmark test/flow_mechanism_benchmark.cpp)
    target_include_directories(flow_mechanism_benchmark PRIVATE src)
    target_link_libraries(flow_mechanism_benchmark PRIVATE aura)
    target_compile_features(flow_mechanism_benchmark PRIVATE cxx_std_17)
    foreach(benchmark IN ITEMS flow_gpu_gap_benchmark flow_diagnostics_benchmark flow_payload_benchmark)
        add_executable(${benchmark} test/${benchmark}.cpp)
        target_include_directories(${benchmark} PRIVATE src)
        target_link_libraries(${benchmark} PRIVATE aura)
        target_compile_features(${benchmark} PRIVATE cxx_std_17)
    endforeach()
endif()
if(TARGET aura_testing AND ENABLE_TEST_XFLOW)
    target_sources(aura_test PRIVATE test/test_flow_mechanisms.cpp)
endif()
'''
p.write_text(text)
manifest = {'ref': commit, 'queue': args.queue, 'records': args.records, 'payload': args.payload,
            'file_sha256': {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sorted(out.rglob('*')) if p.is_file()}}
(out / 'flow-mechanism-variant.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Prepared', commit, args.queue, args.records, args.payload, 'at', out)
