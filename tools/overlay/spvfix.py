"""Turn one nx2android (Ryujinx translator) SPIR-V stage module into what Unity 2018.1.4f1's Vulkan backend expects.
What the nx2android host supplies at draw time and the Unity player will not, and what is done about each (see fix()):
  support buffer (set 0 binding 0)   -> removed when unused; else a Private variable initialised with render scale 1.0 (no scaling), everything else 0
  c1 constant segment UBO             -> a Private variable initialised with the constant data (uint words, bit-cast at each load)
  cbuf UBO c3+N (vec4[4096])          -> set 1, binding given by the caller, array cut to the constant buffer's size
  textures (c2 handle slot N)         -> set 0, binding given by the caller (combined image samplers, as Unity's own)
  clip space Y (NVN upper-left origin; the host flips the viewport) -> gl_Position.y negated at the end of the vertex shader, as Unity's HLSLcc does for Vulkan
  SPIR-V 1.3 header and unused capabilities / extension -> version 1.0 and only the capabilities the code uses (the player's SMOL-V decoder refuses other versions)
"""
import struct, collections
from spvlib import Mod, wstr

KIND = {0: 'support', 1: 'cbuf', 2: 'c1', 7: 'tex'}
OP = dict(Name=5, MemberName=6, Source=3, SourceExt=4, Extension=10, Capability=17, EntryPoint=15, TypeInt=21, TypeFloat=22, TypeVector=23, TypeArray=28,
          TypeStruct=30, TypePointer=32, Constant=43, ConstantComposite=44, Function=54, FunctionEnd=56, Variable=59, Load=61, Store=62, AccessChain=65,
          InBoundsAccessChain=66, Decorate=71, MemberDecorate=72, Bitcast=124, FNegate=127, Return=253)
SC_UNIFORM_CONSTANT, SC_INPUT, SC_UNIFORM, SC_OUTPUT, SC_PRIVATE = 0, 1, 2, 3, 6
DEC_BLOCK, DEC_ARRAY_STRIDE, DEC_BUILTIN, DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_OFFSET = 2, 6, 11, 30, 33, 34, 35
CAP_SHADER, CAP_IMAGE_QUERY, CAP_GATHER_EXT, CAP_CLIP, CAP_CULL, CAP_SAMPLE_RATE, CAP_IMAGE_MS = 1, 50, 25, 32, 33, 35, 27
SUPPORT_RENDER_SCALE_MEMBER, SUPPORT_SCALE_COUNT_MEMBER = 5, 4

class FixError(Exception): pass

def fix(spv, stage, refl, cb_map, tex_map, strip_names=True, pad_outputs=()):
    """spv: module bytes; stage 'vs' / 'ps'; refl: dmsvlib refl dict; cb_map {cbuf reg: (binding, size bytes)}; tex_map {tex reg: binding}.
    -> (new module bytes, info dict: inputs / outputs (location sets), tex {reg: (dim, depth, arrayed, ms)}, cbuf regs, notes)"""
    m = Mod(spv); info = dict(notes=[])
    types, consts, deco, mdeco = {}, {}, collections.defaultdict(dict), collections.defaultdict(dict)
    for x in m.ins:
        op = x[0]
        if op == OP['TypeInt']: types[x[1]] = ('int', x[2], x[3])
        elif op == OP['TypeFloat']: types[x[1]] = ('float', x[2])
        elif op == OP['TypeVector']: types[x[1]] = ('vec', x[2], x[3])
        elif op == OP['TypeArray']: types[x[1]] = ('array', x[2], x[3])
        elif op == OP['TypeStruct']: types[x[1]] = ('struct', x[2:])
        elif op == OP['TypePointer']: types[x[1]] = ('ptr', x[2], x[3])
        elif op == 25: types[x[1]] = ('image', x[2], x[3], x[4], x[5], x[6], x[7])   # sampled type, dim, depth, arrayed, ms, sampled
        elif op == 27: types[x[1]] = ('sampled', x[2])
        elif op == OP['Constant'] and len(x) == 4: consts[x[2]] = (x[1], x[3])
        elif op == OP['Decorate']: deco[x[1]][x[2]] = x[3:]
        elif op == OP['MemberDecorate']: mdeco[x[1]][(x[2], x[3])] = x[4:]
    def find_type(pred):
        return next((i for i, t in types.items() if pred(t)), None)
    t_float = find_type(lambda t: t == ('float', 32))
    t_uint = find_type(lambda t: t == ('int', 32, 0))
    t_int = find_type(lambda t: t == ('int', 32, 1))
    new_globals = []   # instructions to add before the first function
    def new_type(ins):
        """add a type / constant instruction ([op, ...] with the result id at index `ins.rid`), reusing an identical existing one"""
        i = m.new_id(); return i
    def get_int_type(signed):
        nonlocal t_uint, t_int
        if signed and t_int: return t_int
        if not signed and t_uint: return t_uint
        i = m.new_id(); new_globals.append([OP['TypeInt'], i, 32, 1 if signed else 0]); types[i] = ('int', 32, 1 if signed else 0)
        if signed: t_int = i
        else: t_uint = i
        return i
    def get_float():
        nonlocal t_float
        if not t_float: t_float = m.new_id(); new_globals.append([OP['TypeFloat'], t_float, 32]); types[t_float] = ('float', 32)
        return t_float
    cache = {}
    def const_u(v):
        t = get_int_type(False)
        k = ('cu', v)
        if k not in cache:
            ex = next((i for i, (ty, val) in consts.items() if ty == t and val == v), None)
            if ex is None: ex = m.new_id(); new_globals.append([OP['Constant'], t, ex, v])
            cache[k] = ex
        return cache[k]
    def const_f(bits):
        t = get_float(); k = ('cf', bits)
        if k not in cache: i = m.new_id(); new_globals.append([OP['Constant'], t, i, bits]); cache[k] = i
        return cache[k]
    def mk(key, build):
        if key not in cache: i = m.new_id(); new_globals.append(build(i)); cache[key] = i
        return cache[key]
    def ptr(sc, t): return mk(('ptr', sc, t), lambda i: [OP['TypePointer'], i, sc, t])
    def vec(t, n): return mk(('vec', t, n), lambda i: [OP['TypeVector'], i, t, n])
    def arr(t, n): return mk(('arr', t, n), lambda i: [OP['TypeArray'], i, t, const_u(n)])
    def struct_(members): return mk(('struct', tuple(members)), lambda i: [OP['TypeStruct'], i] + list(members))
    def comp(t, parts): return mk(('comp', t, tuple(parts)), lambda i: [OP['ConstantComposite'], t, i] + list(parts))

    # ---- which variable is which: by binding number (the translator numbers every resource in set 0) ----
    kind_of = {b['binding']: b for b in refl['binds']}
    vars_ = {x[2]: x for x in m.ins if x[0] == OP['Variable']}
    res = {}   # variable id -> refl binding
    for vid, x in vars_.items():
        d = deco.get(vid, {})
        if DEC_BINDING in d:
            b = kind_of.get(d[DEC_BINDING][0])
            if b is None or KIND.get(b['kind']) is None: raise FixError(f'binding {d[DEC_BINDING][0]}: resource kind {b and b["kind"]} is not handled')
            if d.get(DEC_SET, [0])[0] != 0: raise FixError('a resource outside descriptor set 0')
            res[vid] = b
    chains = collections.defaultdict(list)   # base variable -> access chain instructions
    for x in m.ins:
        if x[0] in (OP['AccessChain'], OP['InBoundsAccessChain']) and x[3] in res: chains[x[3]].append(x)
    # any other use of a resource variable than access chains (cbuf / c1 / support) or OpLoad (textures) is not expected
    drop_deco, drop_vars, retype_private = set(), set(), {}
    add_deco, pre_var = [], collections.defaultdict(list)
    info['tex'], info['cbuf'] = {}, []
    for vid, b in res.items():
        k = KIND[b['kind']]; x = vars_[vid]; pt = types[x[1]]
        if k == 'tex':
            if b['reg'] not in tex_map: raise FixError(f'texture slot {b["reg"]} has no Unity texture parameter')
            deco[vid][DEC_SET] = [0]; deco[vid][DEC_BINDING] = [tex_map[b['reg']]]
            img = types[types[pt[2]][1]] if types[pt[2]][0] == 'sampled' else types[pt[2]]
            info['tex'][b['reg']] = dict(dim=img[2], depth=img[3], arrayed=img[4], ms=img[5])
        elif k == 'cbuf':
            if b['reg'] not in cb_map: raise FixError(f'constant buffer slot {b["reg"]} has no Unity constant buffer')
            binding, size = cb_map[b['reg']]
            st = types[pt[2]]; a = types[st[1][0]]
            if st[0] != 'struct' or len(st[1]) != 1 or a[0] != 'array' or types[a[1]][0] != 'vec': raise FixError('constant buffer block is not {vec4[]}')
            n = max(1, (size + 15) // 16)
            na = m.new_id(); ns = m.new_id(); np_ = m.new_id()
            pre_var[vid] += [[OP['TypeArray'], na, a[1], None], [OP['TypeStruct'], ns, na], [OP['TypePointer'], np_, SC_UNIFORM, ns]]
            pre_var[vid][0][3] = ('const_u', n)
            add_deco += [[OP['Decorate'], na, DEC_ARRAY_STRIDE, 16], [OP['Decorate'], ns, DEC_BLOCK], [OP['MemberDecorate'], ns, 0, DEC_OFFSET, 0]]
            x[1] = np_
            deco[vid][DEC_SET] = [1]; deco[vid][DEC_BINDING] = [binding]
            info['cbuf'].append(b['reg'])
        elif k in ('c1', 'support'):
            drop_deco.add(vid)
            if not chains[vid]: drop_vars.add(vid); continue
            retype_private[vid] = k
    # ---- c1 / support buffer as initialised Private variables ----
    load_cast = {}   # pointer id (an access chain result) -> (uint-ish type to load, original type)
    for vid, k in retype_private.items():
        x = vars_[vid]; st = types[types[x[1]][2]]
        tu = get_int_type(False); tf = get_float()
        if k == 'c1':
            words = list(struct.unpack(f'<{len(refl["c1"]) // 4}I', refl['c1'][:len(refl['c1']) // 4 * 4])); words += [0] * (-len(words) % 4)
            n = max(1, len(words) // 4)
            words += [0] * (4 * n - len(words))
            uv4 = vec(tu, 4); at = arr(uv4, n); s = struct_([at])
            init = comp(s, [comp(at, [comp(uv4, [const_u(w) for w in words[4 * i:4 * i + 4]]) for i in range(n)])])
            add_deco.append([OP['MemberDecorate'], s, 0, DEC_OFFSET, 0])    # see the note at the support buffer below
            fv4 = types[st[1][0]][1]
            for c in chains[vid]:
                rt = types[c[1]]   # pointer Uniform -> float or vec4
                if len(c) == 6:   # [op, type, id, base, 0, idx]: a whole vec4
                    c[1] = ptr(SC_PRIVATE, uv4); load_cast[c[2]] = (uv4, rt[2])
                elif len(c) == 7: c[1] = ptr(SC_PRIVATE, tu); load_cast[c[2]] = (tu, rt[2])
                else: raise FixError('c1 access chain of an unexpected depth')
                ix = c[5]
                if ix in consts and consts[ix][1] >= n: info['notes'].append(f'c1 read at vec4 {consts[ix][1]} past its {n} vec4s')
            sp = ptr(SC_PRIVATE, s)
        else:
            # support buffer: {uint alphaTest; uint isBgra[8]; vec4 viewportInverse; vec4 viewportSize; int scaleCount; float renderScale[73]; ivec4 tfeOffset; int tfeVertexCount}
            mem = st[1]; parts = []; newm = []
            def zero(t):
                tt = types[t]
                if tt[0] == 'int': return mk(('zero', t), lambda i: [OP['Constant'], t, i, 0])
                if tt[0] == 'float': return const_f(0)
                if tt[0] == 'vec': return comp(t, [zero(tt[1])] * tt[2])
                raise FixError('support buffer member type')
            for i, t in enumerate(mem):
                tt = types[t]
                if tt[0] == 'array':
                    n = consts[tt[2]][1]; el = tt[1]; at = arr(el, n)   # a fresh array type without the ArrayStride decoration
                    one = const_f(0x3f800000) if i == SUPPORT_RENDER_SCALE_MEMBER else zero(el)
                    newm.append(at); parts.append(comp(at, [one] * n))
                else:
                    newm.append(t); parts.append(mk(('one', t), lambda j: [OP['Constant'], t, j, 1]) if i == SUPPORT_SCALE_COUNT_MEMBER else zero(t))
            s = struct_(newm); init = comp(s, parts); sp = ptr(SC_PRIVATE, s)
            # The player's vk::PushConstantTransformation (Mali devices) computes every struct's size from the Offset of its last member and reads through a null
            # pointer when a struct has none (found under Unicorn): our new structs carry the original member offsets, though they are no interface blocks
            old_struct = types[x[1]][2]
            for i in range(len(mem)): add_deco.append([OP['MemberDecorate'], s, i, DEC_OFFSET] + mdeco[old_struct].get((i, DEC_OFFSET), [0]))
            for c in chains[vid]:
                c[1] = ptr(SC_PRIVATE, types[c[1]][2])
            info['notes'].append('support buffer baked (render scale 1)')
        x[1] = sp; x[3] = SC_PRIVATE; x.append(init) if len(x) == 4 else x.__setitem__(4, init)
    # ---- rebuild the instruction list ----
    used_ops = collections.Counter(x[0] for x in m.ins)
    keep_caps = {CAP_SHADER}
    if any(used_ops[o] for o in (103, 104, 105, 106, 107)): keep_caps.add(CAP_IMAGE_QUERY)
    if any(used_ops[o] for o in (96, 97)): keep_caps.add(CAP_GATHER_EXT)
    if any(t[0] == 'image' and t[5] == 1 for t in types.values()) and any(used_ops[o] for o in (95,)): pass   # texelFetch on a multisampled sampled image needs no capability
    entry = next(x for x in m.ins if x[0] == OP['EntryPoint']); entry_fn = entry[2]
    out = []; in_fn = None; emitted_globals = False
    # Y flip pieces
    pos_var = None
    if stage == 'vs':
        for vid, x in vars_.items():
            if x[3] == SC_OUTPUT:
                st = types[types[x[1]][2]]
                if st[0] == 'struct' and mdeco.get(types[x[1]][2], {}).get((0, DEC_BUILTIN)) == [0]: pos_var = vid
        if pos_var is None: raise FixError('vertex shader without a gl_Position block')
        tf = get_float(); pf = ptr(SC_OUTPUT, tf); c0 = const_u(0); c1_ = const_u(1)
        # outputs the paired pixel shader reads and this vertex shader never writes (nx2android's Metal chain pads them too, dmsh_build.py vs_pad): (0, 0, 0, 1),
        # what an unwritten generic attribute reads as; without them the stage interfaces do not match, which Vulkan leaves undefined
        pads = []
        if pad_outputs:
            v4 = vec(tf, 4) if not find_type(lambda t: t == ('vec', tf, 4)) else find_type(lambda t: t == ('vec', tf, 4))
            pv4 = ptr(SC_OUTPUT, v4); dflt = comp(v4, [const_f(0), const_f(0), const_f(0), const_f(0x3f800000)])
            for loc in pad_outputs:
                vid = m.new_id(); new_globals.append([OP['Variable'], pv4, vid, SC_OUTPUT]); add_deco.append([OP['Decorate'], vid, DEC_LOCATION, loc])
                entry.append(vid); pads.append(vid); deco[vid][DEC_LOCATION] = [loc]; vars_[vid] = [OP['Variable'], pv4, vid, SC_OUTPUT]
            info['notes'].append(f'outputs {list(pad_outputs)} added (the pixel shader reads them)')
    for x in m.ins:
        op = x[0]
        if op == OP['Capability'] and x[1] not in keep_caps: continue
        if op == OP['Extension']: continue
        if strip_names and op in (OP['Name'], OP['MemberName'], OP['Source'], OP['SourceExt'], 2, 7, 8): continue
        if op == OP['Decorate']:
            if x[1] in drop_deco and x[2] in (DEC_SET, DEC_BINDING): continue
            if x[1] in res and x[2] == DEC_SET: x = [op, x[1], DEC_SET] + deco[x[1]][DEC_SET]
            elif x[1] in res and x[2] == DEC_BINDING: x = [op, x[1], DEC_BINDING] + deco[x[1]][DEC_BINDING]
        if op == OP['Name'] and x[1] in drop_vars: continue
        if op == OP['Variable'] and x[2] in drop_vars: continue
        if op == OP['Variable'] and x[2] in pre_var:
            for y in pre_var[x[2]]:
                if y[0] == OP['TypeArray'] and isinstance(y[3], tuple): y[3] = const_u(y[3][1])
        if op == OP['Function'] and not emitted_globals:
            emitted_globals = True; out.append('GLOBALS')
        if op == OP['Function']: in_fn = x[2]
        if op == OP['FunctionEnd']: in_fn = None
        if op == OP['Variable'] and x[2] in retype_private and in_fn is None:
            out.append(('LATE', x)); continue      # after the new types and constants it needs
        if op == OP['Variable'] and x[2] in pre_var and in_fn is None:
            out.append(('LATE', pre_var[x[2]] + [x])); continue
        if op == OP['Load'] and x[3] in load_cast:
            ut, ot = load_cast[x[3]]; t = m.new_id()
            out.append([OP['Load'], ut, t, x[3]] + x[4:]); out.append([OP['Bitcast'], ot, x[2], t]); continue
        if op == OP['Return'] and stage == 'vs' and in_fn == entry_fn:
            p, v, nv = m.new_id(), m.new_id(), m.new_id()
            out += [[OP['AccessChain'], pf, p, pos_var, c0, c1_], [OP['Load'], tf, v, p], [OP['FNegate'], tf, nv, v], [OP['Store'], p, nv]]
            for vid in pads: out.append([OP['Store'], vid, dflt])
        out.append(x)
    # decorations must precede types: put the new ones right after the last existing annotation
    final = []; late = []
    for x in out:
        if x == 'GLOBALS':
            final += new_globals
            for y in late: final += y if isinstance(y[0], list) else [y]
            continue
        if isinstance(x, tuple): late.append(x[1]); continue
        final.append(x)
    # insert add_deco after the last OpDecorate / OpMemberDecorate (or before the first type declaration)
    idx = max((i for i, x in enumerate(final) if x[0] in (OP['Decorate'], OP['MemberDecorate'])), default=None)
    if idx is None: idx = next(i for i, x in enumerate(final) if x[0] in (19, 20, 21, 22)) - 1
    final[idx + 1:idx + 1] = add_deco
    m.ins = final; m.ver = 0x00010000
    # interface summary
    locs = {SC_INPUT: set(), SC_OUTPUT: set()}
    for vid, x in vars_.items():
        if x[3] in locs and DEC_LOCATION in deco.get(vid, {}): locs[x[3]].add(deco[vid][DEC_LOCATION][0])
    info['inputs'], info['outputs'] = sorted(locs[SC_INPUT]), sorted(locs[SC_OUTPUT])
    def shape(vid):
        t = types[types[vars_[vid][1]][2]]
        return (types[t[1]][0], t[2]) if t[0] == 'vec' else (t[0], 1)
    info['input_types'] = {deco[v][DEC_LOCATION][0]: shape(v) for v, x in vars_.items() if x[3] == SC_INPUT and DEC_LOCATION in deco.get(v, {})}
    info['builtins'] = sorted(d[DEC_BUILTIN][0] for v, d in deco.items() if DEC_BUILTIN in d and v in vars_)
    return m.bytes(), info
