// Snippets tied to a builtin module. Completion offers them only when the file includes that module,
// and writes the alias the file actually uses (`regex` when the include says `as regex`).

export interface ModuleSnippet {
  prefix: string;
  body: string;
  description: string;
}

type Template = { prefix: string; body: string[]; description: string };

const TEMPLATES: Record<string, Template[]> = {
  regex: [
    { prefix: '$A.expect', description: 'Match with a readable pattern (x digit, X letter, ? any, * run)',
      body: ['if ($A.expect("${1:20xx-xx-xx}", ${2:text})) {', '    ${0}', '}'] },
    { prefix: '$A.match', description: 'Whole text matches a regex',
      body: ['if ($A.match(${1:text}, "${2:[0-9]+}")) {', '    ${0}', '}'] },
  ],
  ai: [
    { prefix: '$A.infer', description: 'Infer a regex from samples',
      body: ['string ${1:rx} = $A.infer.regex([', '    "${2:sample one}",', '    "${3:sample two}"', ']);'] },
    { prefix: '$A.generate', description: 'Build a regex from an English description',
      body: ['string ${1:rx} = $A.regex.generate("${2:3 digits then a dash then 4 letters}");'] },
    { prefix: '$A.explain', description: 'Explain a regex in plain English',
      body: ['print $A.regex.explain(${1:rx});'] },
  ],
  net: [
    { prefix: '$A.get', description: 'HTTP GET and print the body',
      body: ['string ${1:body} = $A.get("${2:https://example.com}");', 'print ${1:body};'] },
  ],
  ws: [
    { prefix: '$A.connect', description: 'Connect and read one message',
      body: ['string ${1:sock} = $A.connect("${2:ws://127.0.0.1:8080}");', 'string ${3:msg} = ${1:sock}.recv_timeout(${4:1000});'] },
  ],
  system: [
    { prefix: '$A.cmd', description: 'Run a command line and use its output',
      body: ['string ${1:out} = $A.cmd("${2:docker ps}");', 'if ($A.last_exit() != 0) {', '    ${0}', '}'] },
  ],
  process: [
    { prefix: '$A.start', description: 'Start a background process and wait for it',
      body: ['string ${1:p} = $A.start("${2:command}");', 'string ${3:code} = ${1:p}.wait(${4:5000});'] },
  ],
};

export function moduleSnippets(moduleId: string, alias: string): ModuleSnippet[] {
  const list = TEMPLATES[moduleId] ?? [];
  return list.map(t => ({
    prefix: t.prefix.replace(/\$A/g, alias),
    body: t.body.join('\n').replace(/\$A/g, alias),
    description: t.description,
  }));
}
