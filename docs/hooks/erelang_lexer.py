"""Pygments lexer for Erelang, mirroring erevos-language/syntaxes/erevos.tmLanguage.json.

MkDocs loads this file as a hook (see mkdocs.yml). It registers the lexer with Pygments so that
code fences tagged elan, erelang or ere are highlighted like the VS Code extension does.
"""
import sys
from pathlib import Path

from pygments.lexer import RegexLexer, bygroups
from pygments.token import (Comment, Keyword, Name, Number, Operator, Punctuation,
                            String, Whitespace)

__all__ = ["ErelangLexer"]

TYPES = (r"void|int|double|float|string|str|bool|char|auto|any|pointer|unit|never|Array|Map|HashMap|"
         r"array|map|hashmap|dictionary|set|Set|u8|u16|u32|u64|i8|i16|i32|i64|uint|unsigned")
CONTROL = (r"if|else|while|repeat|for|switch|match|case|default|break|continue|return|parallel|wait|"
           r"all|fire|pause|try|catch|await|unsafe|namespace|hook|type")
MODIFIERS = r"public|private|export|extern|const|constexpr|static|global|field|async|import|run|let|lambda"
BUILTINS = (r"print|sleep|input|now_ms|now_iso|alloc|free|realloc|copy|move|fill|zero|sizeof|alignof|"
            r"heap|shared|weak|buffer|call_action|env|fail|from_json|to_json")


class ErelangLexer(RegexLexer):
    name = "Erelang"
    aliases = ["elan", "erelang", "ere"]
    filenames = ["*.elan"]
    mimetypes = ["text/x-erelang"]

    tokens = {
        "root": [
            (r"\s+", Whitespace),
            (r"//.*$", Comment.Single),
            (r"@[A-Za-z_]\w*", Keyword.Namespace),
            (r"^(\s*)(#include)(\s*)(<[^>]*>|\"[^\"]*\")?(\s*)(as)?(\s*)([A-Za-z_]\w*)?",
             bygroups(Whitespace, Keyword.Namespace, Whitespace, String, Whitespace, Keyword,
                      Whitespace, Name.Namespace)),
            (r'"', String.Double, "string"),
            (r"0[xX][0-9A-Fa-f]+", Number.Hex),
            (r"\d+\.\d+([eE][+-]?\d+)?", Number.Float),
            (r"\d+(ms|s|m|h)?\b", Number.Integer),
            (r"\b(true|false|null|nil|nullptr)\b", Keyword.Constant),
            (r"\b(entity|struct|enum|trait)(\s+)([A-Za-z_]\w*)",
             bygroups(Keyword.Declaration, Whitespace, Name.Class)),
            (r"\b(new)(\s+)([A-Za-z_]\w*)", bygroups(Keyword, Whitespace, Name.Class)),
            (r"\b(action)(\s+)([A-Za-z_]\w*)", bygroups(Keyword.Declaration, Whitespace, Name.Function)),
            (r"\b(%s)\b" % CONTROL, Keyword),
            (r"\b(%s)\b" % MODIFIERS, Keyword.Declaration),
            (r"\b(Option|Result|Array|Map|Set|HashMap)(?=<)", Keyword.Type),
            (r"\b(%s)\b" % TYPES, Keyword.Type),
            (r"\b(%s)\b" % BUILTINS, Name.Builtin),
            (r"\b([A-Za-z_]\w*)(::)", bygroups(Name.Namespace, Punctuation)),
            (r"([A-Za-z_]\w*)(?=\s*\()", Name.Function),
            (r"\b[A-Z][A-Za-z0-9_]*\b", Name.Class),
            (r"[A-Za-z_]\w*", Name),
            (r"==|!=|<=|>=|\+=|-=|\*=|/=|%=|&&|\|\||->|=>|[-+*/%<>=!&|^~?]", Operator),
            (r"[;,:.(){}\[\]]", Punctuation),
        ],
        "string": [
            (r"\\.", String.Escape),
            (r"\{[A-Za-z_]\w*\}", String.Interpol),
            (r'[^"\\{]+', String.Double),
            (r"\{", String.Double),
            (r'"', String.Double, "#pop"),
        ],
    }


def on_config(config, **kwargs):
    """MkDocs hook: make Pygments (and so pymdownx.highlight) aware of the Erelang lexer."""
    here = str(Path(__file__).parent)
    if here not in sys.path:
        sys.path.insert(0, here)
    from pygments.lexers import LEXERS
    LEXERS["ErelangLexer"] = ("erelang_lexer", "Erelang", tuple(ErelangLexer.aliases),
                              tuple(ErelangLexer.filenames), tuple(ErelangLexer.mimetypes))
    return config
