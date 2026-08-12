// =============================================================================
// Erelang Semantic Tokens — defined actions as functions, entities as classes
// =============================================================================

import * as vscode from 'vscode';
import { ACTION_RE, TYPED_FUNC_RE } from './constants';
import { collectUserTypeNames, scanWords } from './symbols';

const legend = new vscode.SemanticTokensLegend(
  ['function', 'class'],
  ['declaration', 'defaultLibrary'],
);

const TOKEN_FUNCTION = 0;
const TOKEN_CLASS    = 1;

// ─── Cached defined-action collection ───────────────────────────────────────

const _defCache = new Map<string, { version: number; value: Set<string> }>();

/** Collect every action / typed-function name declared in this document. Cached per doc version. */
export function collectDefinedActions(doc: vscode.TextDocument): Set<string> {
  if (doc.isClosed) return new Set();
  const key = doc.uri.toString();
  const cached = _defCache.get(key);
  if (cached && cached.version === doc.version) return cached.value;
  return collectDefinedActionsUncached(doc);
}

export function collectDefinedActionsUncached(doc: vscode.TextDocument): Set<string> {
  const names = new Set<string>();
  try {
    if (doc.isClosed) return names;
    for (let i = 0; i < doc.lineCount; i++) {
      const text = doc.lineAt(i).text;
      const am = ACTION_RE.exec(text);
      if (am) names.add(am[1]);
      const tm = TYPED_FUNC_RE.exec(text);
      if (tm) names.add(tm[1]);
    }
  } catch { /* ignore */ }
  _defCache.set(doc.uri.toString(), { version: doc.version, value: names });
  return names;
}

/** Invalidate the defined-actions cache for a document (call on every change). */
export function invalidateDefCache(docUri?: string): void {
  if (docUri) _defCache.delete(docUri); else _defCache.clear();
}

// ─── Provider ───────────────────────────────────────────────────────────────

export class ErelangSemanticTokensProvider implements vscode.DocumentSemanticTokensProvider {
  private readonly _tokens = new Map<string, { version: number; at: number; tokens: vscode.SemanticTokens }>();

  provideDocumentSemanticTokens(
    doc: vscode.TextDocument,
    token: vscode.CancellationToken,
  ): vscode.ProviderResult<vscode.SemanticTokens> {
    const key = doc.uri.toString();
    const hit = this._tokens.get(key);
    if (hit && (hit.version === doc.version || Date.now() - hit.at < 250)) return hit.tokens;

    const builder = new vscode.SemanticTokensBuilder(legend);
    try {
      if (doc.isClosed) return builder.build();
      const defined = collectDefinedActions(doc);
      const types   = collectUserTypeNames(doc);

      for (let i = 0; i < doc.lineCount; i++) {
        if (token.isCancellationRequested) break;

        const text = doc.lineAt(i).text;
        const trimmed = text.trimStart();
        if (trimmed.startsWith('//') || trimmed.startsWith('#')) continue;
        const lineLen = text.length;

        const pushTok = (col: number, len: number, type: number, mods: number) => {
          if (col < 0 || len <= 0 || col >= lineLen) return;
          const clamped = Math.min(len, lineLen - col);
          if (clamped <= 0) return;
          builder.push(i, col, clamped, type, mods);
        };

        const am = ACTION_RE.exec(text);
        if (am) {
          const col = text.indexOf(am[1], am.index);
          pushTok(col, am[1].length, TOKEN_FUNCTION, 1);
        }
        const tm = TYPED_FUNC_RE.exec(text);
        if (tm) {
          const col = text.indexOf(tm[1], tm.index);
          pushTok(col, tm[1].length, TOKEN_FUNCTION, 1);
        }

        for (const w of scanWords(text)) {
          if (w.start > 0 && text[w.start - 1] === '.') continue;
          if (types.has(w.text)) pushTok(w.start, w.length, TOKEN_CLASS, 0);
        }

        const callRe = /\b([A-Za-z_]\w*)\s*\(/g;
        let m: RegExpExecArray | null;
        while ((m = callRe.exec(text))) {
          const name = m[1];
          if (types.has(name) || !defined.has(name)) continue;
          if (name === 'if' || name === 'while' || name === 'for' || name === 'switch' || name === 'match' || name === 'catch') continue;
          pushTok(m.index, name.length, TOKEN_FUNCTION, 0);
        }
      }
    } catch {
      /* never throw into the extension host */
    }
    const tokens = builder.build();
    this._tokens.set(key, { version: doc.version, at: Date.now(), tokens });
    return tokens;
  }
}

export { legend as erelangSemanticLegend };
