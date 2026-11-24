export interface CollectedSymbols {
  entities:     Set<string>;
  actions:      Set<string>;
  fields:       Set<string>;
  hooks:        Set<string>;
  globals:      Set<string>;
  locals:       Set<string>;
  arrays:       Set<string>;
  dictionaries: Set<string>;
  structs:      Set<string>;
  enums:        Set<string>;
  typeAliases:  Set<string>;
  structFields: Map<string, Set<string>>;
  enumMembers:  Map<string, Set<string>>;
  namespaces:   Set<string>;
  namespaceMembers: Map<string, Set<string>>;
}

export interface EntityMembers {
  actions: Map<string, Set<string>>;
  fields:  Map<string, Set<string>>;
}

/** A typed declaration: `Array<int> xs`, `string s = ...`, a parameter `n: int`, a loop variable. */
export interface Declaration {
  type: string;
  line: number;
  /** Right-hand side text when there is one, used to spot handle-returning calls. */
  init?: string;
}

export interface DocumentIndex {
  version: number;
  /** Declarations by variable name, in source order. */
  declarations: Map<string, Declaration[]>;
  symbols: CollectedSymbols;
  entityInstances: Map<string, string>;
  entityMembers: EntityMembers;
  outline: OutlineSymbol[];
}

export interface OutlineSymbol {
  name: string;
  kind: 'entity' | 'action' | 'field' | 'hook' | 'namespace';
  line: number;
}

export interface ImportedSymbols {
  aliasToActions: Map<string, Set<string>>;
  /** Alias of a builtin module include -> normalized module path (builtin/regex). */
  aliasToModule:  Map<string, string>;
  allActions:     Set<string>;
}

export type PrintStringContext = {
  interpolation:            boolean;
  partial:                  string;
  replaceStart:             number;
  replaceEnd:               number;
  shouldAppendClosingBrace: boolean;
};

export type WordToken = { text: string; start: number; length: number };
export type RangeToken = { start: number; length: number };
