#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Production colour-mask blocks from StateUpdater.cs with recording endpoints; no GPU.

Called by tests/gpu_omap_mask_state_test.sh. Needs only dotnet and python3.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

MUTATIONS = ['and', 'stored', 'clear-reset', 'clear-masks', 'clear-dirty', 'site-comment', 'site-guard', 'site-forward']


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def line(text, marker):
    start = text.index(marker)
    return text[start:text.index('\n', start)]


def code_lines(text):
    """Statements of a block with // comments and blank lines removed."""
    out = []
    for raw in text.splitlines():
        stripped = re.sub(r'//.*$', '', raw).strip()
        if stripped:
            out.append(stripped)
    return out


def call_site_error(owner, body, call, later):
    """The call must be a statement of its own, exactly once, unguarded, before `later`."""
    lines = code_lines(body)
    hits = [i for i, text in enumerate(lines) if text == call]
    if len(hits) != 1:
        return f'{owner} must contain the statement {call} exactly once, found {len(hits)}'
    if later is not None:
        after = [i for i, text in enumerate(lines) if text.startswith(later)]
        if not after or hits[0] > after[0]:
            return f'{owner} must call {call} before {later}'
    if re.match(r'(if|else|while|for|foreach|switch|case|return)\b', lines[hits[0] - 1]):
        return f'{owner}: {call} sits behind "{lines[hits[0] - 1]}"'
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dotnet', default='dotnet')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--source-root', type=Path)
    p.add_argument('--mutation', choices=MUTATIONS)
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    repo = a.source_root or here.parents[1]
    threed = repo / 'compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed'
    path = threed / 'StateUpdater.cs'
    source = path.read_text()
    draw_manager = (threed / 'DrawManager.cs').read_text()
    threed_class = (threed / 'ThreedClass.cs').read_text()
    clear = 'public void S2AClearFragmentOutputMap()'
    blocks = {
        '// FIELD_DECLARATION': line(source, 'private uint _s2aFragmentOutputMap'),
        '// MASK_FUNCTION': block(source, 'internal static uint S2AOmapColorMask('),
        '// UPDATE_MASK_METHOD': block(source, 'private void UpdateRtColorMask()'),
        # Sources without the method get an empty stand-in, so the old behaviour fails at the assertions,
        # not in the compiler.
        '// CLEAR_MAP_METHOD': block(source, clear) if clear in source else clear + ' { }',
    }
    send = 'componentMasks[index] = componentMask;'
    store = 'colorWriteMaskSpan[index] = componentMask;'
    controls = {
        'and': ('// UPDATE_MASK_METHOD', 'componentMask = S2AOmapColorMask(componentMask, _s2aFragmentOutputMap, index);', ''),
        'clear-reset': ('// CLEAR_MAP_METHOD', '_s2aFragmentOutputMap = uint.MaxValue;', ''),
        'clear-masks': ('// CLEAR_MAP_METHOD', 'UpdateRtColorMask();', ''),
        'clear-dirty': ('// CLEAR_MAP_METHOD', '_updateTracker.ForceDirty(ShaderStateIndex);', ''),
    }
    call = 'engine.S2AClearFragmentOutputMap();'
    if a.mutation == 'site-comment':
        assert draw_manager.count(call) == 1
        draw_manager = draw_manager.replace(call, '// ' + call)
    elif a.mutation == 'site-guard':
        assert draw_manager.count(call) == 1
        draw_manager = draw_manager.replace(call, 'if (false)\n' + call)
    elif a.mutation == 'site-forward':
        forward = '_stateUpdater.S2AClearFragmentOutputMap();'
        assert threed_class.count(forward) == 1
        threed_class = threed_class.replace(forward, '')
    elif a.mutation == 'stored':
        # Store the masked value, as the code did before this test existed.
        body = blocks['// UPDATE_MASK_METHOD']
        assert body.count(store) == 1 and body.count(send) == 1
        blocks['// UPDATE_MASK_METHOD'] = body.replace(store, '').replace(send, send + ' ' + store)
    elif a.mutation:
        slot, before, after = controls[a.mutation]
        assert blocks[slot].count(before) == 1, 'mutation anchor not found exactly once: ' + a.mutation
        blocks[slot] = blocks[slot].replace(before, after)
    # The callers are too large to cut out; check the call sites by text (comments stripped, whole statements).
    if clear in source:
        errors = [
            call_site_error('DrawManager.DrawTexture', block(draw_manager, 'public void DrawTexture(ThreedClass engine, int argument)'),
                            call, 'engine.UpdateState('),
            call_site_error('ThreedClass.S2AClearFragmentOutputMap', block(threed_class, clear) if clear in threed_class else '',
                            '_stateUpdater.S2AClearFragmentOutputMap();', None),
        ]
        for error in errors:
            if error:
                print('call site check failed: ' + error)
                return 1
    text = (here / 'Fixture.cs.in').read_text()
    for slot, body in blocks.items():
        text = text.replace(slot, body.replace('internal static uint S2AOmapColorMask', 'static uint S2AOmapColorMask'))
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'Program.cs').write_text(text)
    (a.out / 'Proof.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><NuGetAudit>false</NuGetAudit><ImportDirectoryBuildProps>false</ImportDirectoryBuildProps><ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets></PropertyGroup></Project>')
    (a.out / 'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
    results = []
    for name, cmd in [('build', [a.dotnet, 'build', str(a.out / 'Proof.csproj'), '-c', 'Release', '--configfile', str(a.out / 'NuGet.Config')]), ('run', [a.dotnet, str(a.out / 'bin/Release/net10.0/Proof.dll')])]:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        (a.out / (name + '.log')).write_text(r.stdout + r.stderr)
        results.append({'command': cmd, 'exit': r.returncode})
        if r.returncode:
            print(name + ' failed: ' + (r.stdout[-1500:] + r.stderr[-1500:]).strip())
            break
    (a.out / 'result.json').write_text(json.dumps({'sourceSha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'mutation': a.mutation, 'failedStep': name if r.returncode else None, 'commands': results}, indent=2))
    if not r.returncode:
        print(r.stdout.strip())
    return int(r.returncode != 0)


if __name__ == '__main__':
    raise SystemExit(main())
