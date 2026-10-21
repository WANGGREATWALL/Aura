#!/usr/bin/env python3
"""Prepare a separate f0f523c tree with test-only graph timing instrumentation.

Usage: python3 test/prepare_flow_graph_baseline.py /tmp/aura-b3-graph-baseline
The destination must not exist. No network access or repository mutation.
"""
from pathlib import Path
import argparse
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
base = args.destination.resolve()
current = Path(__file__).resolve().parents[1]
base.mkdir(parents=True, exist_ok=False)
archive = subprocess.check_output(["git", "archive", "f0f523c"], cwd=current)
subprocess.run(["tar", "-x", "-C", str(base)], input=archive, check=True)

p=base/'src/flow/xflow.cpp'; original=subprocess.check_output(['git','show','f0f523c:src/flow/xflow.cpp'],cwd=current,text=True);s=original
marker='class AU_LOCAL Core;';assert marker in s
s=s.replace(marker,marker+'''
#ifdef AURA_FLOW_TESTING
void recordGraphSearch(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
#endif
''',1)
needle='''                 bool completion) const
    {
''';assert needle in s
s=s.replace(needle,needle+'''#ifdef AURA_FLOW_TESTING
        struct Probe {
            bool mEnabled;
            std::uint64_t mNodes = 0, mEdges = 0, mScans = 0;
            std::chrono::steady_clock::time_point mStarted;
            explicit Probe(bool enabled) : mEnabled(enabled) {
                if (enabled) mStarted = std::chrono::steady_clock::now();
            }
            ~Probe() {
                if (mEnabled) au::flow::detail::recordGraphSearch(mNodes, mEdges, mScans,
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - mStarted).count()));
            }
        } probe(completion);
#endif
''')
s=s.replace('au::flow::detail::Node* node = todo.back(); todo.pop_back();','''au::flow::detail::Node* node = todo.back(); todo.pop_back();
#ifdef AURA_FLOW_TESTING
            ++probe.mNodes;
#endif''',1)
for old in ('for (const auto& pred : node->mPreds) todo.push_back(pred.get());','for (const auto& member : node->mMembers) todo.push_back(member.get());'):
 assert old in s
 head,body=old.split(') todo',1)
 s=s.replace(old,head+''') {
#ifdef AURA_FLOW_TESTING
                ++probe.mEdges;
#endif
                todo'''+body+'\n            }',1)
for member in ('mEntry','mGate','mWaitTarget','mExecutingChild'):
 old=f'if (node->{member}'+('.get()' if member in ('mEntry','mGate') else '')+') todo.push_back(node->'+member+('.get()' if member in ('mEntry','mGate') else '')+');'
 assert old in s,old
 cond,action=old.split(') todo',1)
 s=s.replace(old,cond+''') {
#ifdef AURA_FLOW_TESTING
                ++probe.mEdges;
#endif
                todo'''+action+'\n            }',1)
old='''                for (au::flow::detail::Node* child = mActive; child; child = child->mActiveNext)
                    if (child->mParent.get() == node) todo.push_back(child);''';assert old in s
s=s.replace(old,'''                for (au::flow::detail::Node* child = mActive; child; child = child->mActiveNext) {
#ifdef AURA_FLOW_TESTING
                    ++probe.mScans;
#endif
                    if (child->mParent.get() == node) {
#ifdef AURA_FLOW_TESTING
                        ++probe.mEdges;
#endif
                        todo.push_back(child);
                    }
                }''')
s=s.replace('void WorkerEntry::operator()() const noexcept { mCore->workerLoop(mId); }','''#ifdef AURA_FLOW_TESTING
std::size_t graphNodeSize() noexcept { return sizeof(au::flow::detail::Node); }
#endif
void WorkerEntry::operator()() const noexcept { mCore->workerLoop(mId); }''')
s=s.replace('#include "flow/testing.h"', '#include "flow/testing.h"\n#include "flow/testing_internal.h"')
a=s.index('    int wait(');i=s.index('std::lock_guard<std::mutex> lock(mMutex);',a)+len('std::lock_guard<std::mutex> lock(mMutex);')
s=s[:i]+'\n#ifdef AURA_FLOW_TESTING\n                au::flow::detail::GraphWaitLock graphWaitLock;\n#endif'+s[i:]
p.write_text(s)
(base/'src/flow/testing_internal.h').write_text((current/'src/flow/testing_internal.h').read_text())
(base/'src/flow/testing.h').write_text((current/'src/flow/testing.h').read_text())
metrics=(current/'src/flow/testing.cpp').read_text();i=metrics.index('std::atomic<std::uint64_t> gSearches');j=metrics.index('std::array<std::atomic<int>, 5>',i)
body=metrics[i:j]
wrapper=re.search(r'au::flow::testing::GraphStats au::flow::testing::graphStats\(bool reset\) noexcept\n\{.*?\n\}',metrics,re.S).group(0).replace('sizeof(au::flow::detail::Node)','au::flow::detail::graphNodeSize()')
(base/'src/flow/graph_metrics.cpp').write_text('''#include "flow/xflow.h"
#include "flow/testing.h"
#include <atomic>
#ifdef AURA_FLOW_TESTING
namespace au { namespace flow AU_LOCAL { namespace detail {
std::size_t graphNodeSize() noexcept;
'''+body+'\n} } }\n'+wrapper+'\n#endif\n')
(base/'test/flow_graph_benchmark.cpp').write_text((current/'test/flow_graph_benchmark.cpp').read_text())
p=base/'CMakeLists.txt';s=subprocess.check_output(['git','show','f0f523c:CMakeLists.txt'],cwd=current,text=True).replace('    src/flow/xflow.cpp\n','    src/flow/xflow.cpp\n    src/flow/graph_metrics.cpp\n');needle='    target_compile_features(flow_microbenchmark PRIVATE cxx_std_17)';assert needle in s
s=s.replace(needle,needle+'''
    if(TARGET aura_testing AND ENABLE_TEST_XFLOW)
        add_executable(flow_graph_benchmark test/flow_graph_benchmark.cpp)
        target_include_directories(flow_graph_benchmark PRIVATE src)
        target_link_libraries(flow_graph_benchmark PRIVATE aura_testing)
        target_compile_features(flow_graph_benchmark PRIVATE cxx_std_17)
    endif()''');p.write_text(s)
# Keep instrumentation patch for review; no scheduler algorithm change in B3.
import difflib
patch = ''.join(difflib.unified_diff(original.splitlines(keepends=True), (base/'src/flow/xflow.cpp').read_text().splitlines(keepends=True), fromfile='a/src/flow/xflow.cpp', tofile='b/src/flow/xflow.cpp'))
(base/'flow-graph-baseline-instrumentation.patch').write_text(patch)
print('Prepared frozen B3 graph measurement tree:', base)
