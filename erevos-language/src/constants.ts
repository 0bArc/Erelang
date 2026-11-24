export const ENTITY_RE       = /^\s*(?:public|private|export)?\s*entity\s+([A-Za-z_]\w*)/;
export const NAMESPACE_RE    = /^\s*namespace\s+([A-Za-z_]\w*)/;
export const ACTION_RE       = /^\s*(?:public|private|export)?\s*(?:async\s+)?action\s+([A-Za-z_]\w*)/;
export const TYPED_FUNC_RE   =
  /^\s*(?:public|private|export)?\s*(?:async\s+)?(?:void|int|double|float|string|str|bool|char|auto|any|pointer|Array(?:<[^>\n]{0,80}>)?|Map(?:<[^>\n]{0,80}>)?|HashMap(?:<[^>\n]{0,80}>)?|[A-Za-z_]\w*(?:<[^>\n]{0,80}>)?)\s+([A-Za-z_]\w*)\s*(?=\()/;
export const FIELD_RE        = /^\s*(?:public|private)?\s*field\s+([A-Za-z_]\w*)/;
export const STRUCT_RE       = /^\s*(?:public|private|export)?\s*struct\s+([A-Za-z_]\w*)/;
export const ENUM_RE         = /^\s*(?:public|private|export)?\s*enum\s+([A-Za-z_]\w*)/;
export const TYPE_ALIAS_RE   = /^\s*(?:public|private|export)?\s*type\s+([A-Za-z_]\w*)/;
export const TRAIT_RE        = /^\s*(?:public|private|export)?\s*trait\s+([A-Za-z_]\w*)/;
export const HOOK_RE         = /^\s*hook\s+([A-Za-z_]\w*)/;
export const LET_RE          = /^\s*(?:let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap|Array<[^>\n]{0,80}>|Map<[^>\n]{0,80}>|HashMap<[^>\n]{0,80}>|[A-Za-z_]\w*<[^>\n]{0,80}>)\s+([A-Za-z_]\w*)/;
export const GLOBAL_RE       = /^\s*(?:public|private|export)?\s*global\s+([A-Za-z_]\w*)/;
export const INCLUDE_ALIAS_RE = /^\s*#\s*include\s*(<[^>]+>|"[^"]+"|[^\s;]+)\s*(?:as\s+([A-Za-z_]\w*))?\s*;?\s*$/;
// Matches: import <path> as alias  |  import "path" as alias  |  import 'path' as alias  |  import bareident as alias
export const IMPORT_ALIAS_RE  = /^\s*import\s+(?:<([^>]+)>|"([^"]+)"|'([^']+)'|([A-Za-z_][\w./-]*))\s*(?:as\s+([A-Za-z_]\w*))?/;

// Builtin modules, members, keywords and type words come from the generated manifest (manifest.ts).
