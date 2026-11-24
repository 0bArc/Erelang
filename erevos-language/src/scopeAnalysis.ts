// Shared lexical binding index for editor features. No workspace-wide spelling replacements.
export interface SourceToken { text: string; line: number; start: number; scope: number; innerScope?: number; }
export interface Binding { token: SourceToken; kind: 'local' | 'parameter' | 'action'; detail?: string; scope: number; }
interface Scope { parent: number; }
export interface BindingIndex {
  tokens: SourceToken[];
  bindings: Binding[];
  scopes: Scope[];
  resolve(token: SourceToken): Binding | undefined;
}

const cache = new Map<string, BindingIndex>();
export function useCompilerTokens(text: string, output: string): void {
  const lines = text.split('\n');
  const tokens: SourceToken[] = [];
  for (const row of output.split('\n')) {
    const match = /^!token\t(\d+)\t(\d+)\t([^\r\n]+)\r?$/.exec(row);
    if (!match) continue;
    const line = Number(match[1]) - 1;
    const bytes = Number(match[2]) - 1;
    if (!lines[line] || bytes < 0) continue;
    const start = Buffer.from(lines[line], 'utf8').subarray(0, bytes).toString('utf8').length;
    tokens.push({ text: match[3].replace(/\r$/, ''), line, start, scope: 0 });
  }
  if (tokens.length) { cache.delete(text); bindingIndex(text, tokens); }
}
export function bindingIndex(text: string, compilerTokens?: SourceToken[]): BindingIndex {
  const hit = cache.get(text);
  if (hit) return hit;
  const tokens: SourceToken[] = [];
  const scopes: Scope[] = [{ parent: -1 }];
  let scope = 0, line = 0, start = 0, i = 0;
  const advance = () => { if (text[i++] === '\n') { line++; start = i; } };
  if (compilerTokens) {
    for (const token of compilerTokens) {
      token.scope = scope;
      tokens.push(token);
      if (token.text === '{') { scopes.push({ parent: scope }); scope = scopes.length - 1; token.innerScope = scope; }
      if (token.text === '}') scope = Math.max(0, scopes[scope].parent);
    }
  }
  while (!compilerTokens && i < text.length) {
    const ch = text[i];
    if (/\s/.test(ch)) { advance(); continue; }
    if (text.startsWith('//', i)) { while (i < text.length && text[i] !== '\n') advance(); continue; }
    if (text.startsWith('/*', i)) {
      advance(); advance();
      while (i < text.length && !text.startsWith('*/', i)) advance();
      if (i < text.length) { advance(); advance(); }
      continue;
    }
    if (ch === '"' || ch === "'") {
      advance();
      while (i < text.length) {
        if (text[i] === '\\') { advance(); if (i < text.length) advance(); }
        else if (text[i] === ch) { advance(); break; }
        else advance();
      }
      continue;
    }
    const begin = i;
    if (/[A-Za-z_]/.test(ch)) { advance(); while (i < text.length && /\w/.test(text[i])) advance(); }
    else { advance(); if ((ch === ':' && text[i] === ':') || (ch === '-' && text[i] === '>')) advance(); }
    tokens.push({ text: text.slice(begin, i), line, start: begin - start, scope });
    if (ch === '{') { scopes.push({ parent: scope }); scope = scopes.length - 1; tokens[tokens.length - 1].innerScope = scope; }
    if (ch === '}') scope = scopes[scope].parent < 0 ? 0 : scopes[scope].parent;
  }
  const bindings: Binding[] = [];
  const declaredTokens = new Set<SourceToken>();
  const ident = (t?: SourceToken) => !!t && /^[A-Za-z_]\w*$/.test(t.text);
  const add = (token: SourceToken, kind: Binding['kind'], owner = token.scope, detail?: string) => {
    if (!declaredTokens.has(token)) {
      declaredTokens.add(token);
      bindings.push({ token, kind, scope: owner, detail });
    }
  };
  const forbidden = new Set(['return', 'run', 'print', 'if', 'while', 'for', 'new', 'else', 'case', 'throw', 'await', 'namespace', 'entity', 'struct', 'enum', 'type', 'hook']);
  for (let n = 0; n < tokens.length; n++) {
    const t = tokens[n];
    // Anonymous function parameters and loop/catch bindings belong to their body.
    if (['lambda', 'for', 'catch'].includes(t.text) && tokens[n + 1]?.text === '(') {
      let end = n + 2, depth = 1;
      for (; end < tokens.length; end++) {
        if (tokens[end].text === '(') depth++;
        if (tokens[end].text === ')' && --depth === 0) break;
      }
      const body = tokens[end + 1];
      if (body?.text === '{' && body.innerScope !== undefined) {
        if (t.text === 'for') {
          const header = tokens.slice(n + 2, end);
          const separator = header.findIndex(x => x.text === ':' || x.text === 'in' || x.text === ';');
          const left = header.slice(0, separator < 0 ? 0 : separator);
          for (let p = 0; p < left.length; p++) {
            if (ident(left[p]) && [',', '=', undefined].includes(left[p + 1]?.text))
              add(left[p], 'local', body.innerScope, p > 0 ? left[p - 1].text : undefined);
          }
        } else {
          const params = tokens.slice(n + 2, end);
          for (let p = 0; p < params.length; p++) {
            if (ident(params[p]) && [':', ',', undefined].includes(params[p + 1]?.text))
              add(params[p], 'parameter', body.innerScope,
                params[p + 1]?.text === ':' ? params[p + 2]?.text : p > 0 ? params[p - 1].text : undefined);
          }
        }
      }
    }
    // Both action name(params) and ReturnType name(params). Reject member calls.
    const action = t.text === 'action' && ident(tokens[n + 1]);
    const typedFunction = ident(t) && !forbidden.has(t.text) && ident(tokens[n + 1]) && tokens[n + 2]?.text === '(';
    if (action || typedFunction) {
      const name = tokens[n + 1];
      let lp = n + 2;
      // Skip generic type parameters in action headers.
      if (tokens[lp]?.text === '<') { while (lp < tokens.length && tokens[lp].text !== '>') lp++; lp++; }
      let end = lp, depth = 0;
      if (tokens[lp]?.text === '(') {
        for (; end < tokens.length; end++) {
          if (tokens[end].text === '(') depth++;
          if (tokens[end].text === ')' && --depth === 0) break;
        }
      } else end = lp - 1;
      let body = end + 1;
      while (body < tokens.length && !['{', ';', '}'].includes(tokens[body].text)) body++;
      if (tokens[body]?.text !== '{') continue;
      const bodyScope = tokens[body + 1]?.scope;
      if (bodyScope === undefined) continue;
      add(name, 'action', t.scope, tokens.slice(n, body).map(x => x.text).join(' '));
      let group = lp + 1, angle = 0;
      for (let p = group; p <= end; p++) {
        if (tokens[p]?.text === '<') angle++;
        if (tokens[p]?.text === '>') angle--;
        if (p !== end && !(tokens[p].text === ',' && angle === 0)) continue;
        const parts = tokens.slice(group, p);
        const colon = parts.findIndex(x => x.text === ':');
        const eq = parts.findIndex(x => x.text === '=');
        const head = eq < 0 ? parts : parts.slice(0, eq);
        const param = colon >= 0 ? parts[colon - 1] : head[head.length - 1];
        if (ident(param)) add(param, 'parameter', bodyScope,
          (colon >= 0 ? head.slice(colon + 1) : head.slice(0, -1)).map(x => x.text).join(''));
        group = p + 1;
      }
      n = end;
      continue;
    }
    if (t.text === 'let' || t.text === 'const') {
      const name = tokens[n + 1];
      if (ident(name) && ['=', ':', ';'].includes(tokens[n + 2]?.text)) {
        add(name, 'local', t.scope, tokens[n + 2]?.text === ':' ? tokens[n + 3]?.text : undefined);
      }
    } else if (ident(t) && !forbidden.has(t.text)) {
      let next = n + 1;
      if (tokens[next]?.text === '<') {
        let depth = 1;
        while (++next < tokens.length && depth) {
          if (tokens[next].text === '<') depth++;
          if (tokens[next].text === '>') depth--;
        }
      }
      const name = tokens[next];
      if (ident(name) && ['=', ';'].includes(tokens[next + 1]?.text)
        && !['.', '::', ':'].includes(tokens[n - 1]?.text)) {
        add(name, 'local', t.scope, tokens.slice(n, next).map(x => x.text).join(''));
      }
    }
  }
  const declaredByToken = new Map(bindings.map(b => [b.token, b]));
  const tokenPositions = new Map(tokens.map((t, n) => [t, n]));
  const byScope = new Map<number, Map<string, Binding[]>>();
  for (const b of bindings) {
    if (!byScope.has(b.scope)) byScope.set(b.scope, new Map());
    const names = byScope.get(b.scope)!;
    if (!names.has(b.token.text)) names.set(b.token.text, []);
    names.get(b.token.text)!.push(b);
  }
  const resolve = (token: SourceToken): Binding | undefined => {
    const declared = declaredByToken.get(token);
    if (declared) return declared;
    const at = tokenPositions.get(token) ?? -1;
    if (['.', '::'].includes(tokens[at - 1]?.text)) return undefined;
    for (let s = token.scope; s >= 0; s = scopes[s].parent) {
      const matches = (byScope.get(s)?.get(token.text) ?? []).filter(b =>
        b.kind === 'action' || b.kind === 'parameter' || b.token.line < token.line
          || b.token.line === token.line && b.token.start <= token.start);
      if (matches.length) return matches[matches.length - 1];
    }
    return undefined;
  };
  const result = { tokens, scopes, bindings, resolve };
  if (cache.size >= 8) cache.delete(cache.keys().next().value!);
  cache.set(text, result);
  return result;
}

export function tokenAt(index: BindingIndex, pos: { line: number; character: number }): SourceToken | undefined {
  return index.tokens.find(t => t.line === pos.line && pos.character >= t.start
    && pos.character <= t.start + t.text.length && /^[A-Za-z_]\w*$/.test(t.text));
}

export function boundReferences(index: BindingIndex, binding: Binding): SourceToken[] {
  return index.tokens.filter(t => t.text === binding.token.text && index.resolve(t) === binding);
}

export function visibleBindings(text: string, pos: { line: number; character: number }): Binding[] {
  const index = bindingIndex(text);
  // Inject an identifier at the cursor to resolve each candidate in that lexical scope.
  const before = index.tokens.filter(t => t.line < pos.line || t.line === pos.line && t.start <= pos.character);
  const last = before[before.length - 1];
  let scope = last?.scope ?? 0;
  if (last?.text === '{') scope = last.innerScope ?? scope;
  if (last?.text === '}') scope = index.scopes[scope].parent;
  return index.bindings.filter(b => index.resolve({ text: b.token.text, line: pos.line, start: pos.character, scope: Math.max(0, scope) }) === b);
}
