// =============================================================================
// Erelang — Constants: regex patterns, keyword lists, method lists
// =============================================================================

// ─── Declaration Patterns ────────────────────────────────────────────────────

export const ENTITY_RE       = /^\s*(?:public|private)?\s*entity\s+([A-Za-z_]\w*)/;
export const ACTION_RE       = /^\s*(?:public|private)?\s*(?:async\s+)?action\s+([A-Za-z_]\w*)/;
export const FIELD_RE        = /^\s*(?:public|private)?\s*field\s+([A-Za-z_]\w*)/;
export const STRUCT_RE       = /^\s*(?:public|private|export)?\s*struct\s+([A-Za-z_]\w*)/;
export const ENUM_RE         = /^\s*(?:public|private|export)?\s*enum\s+([A-Za-z_]\w*)/;
export const TYPE_ALIAS_RE   = /^\s*(?:public|private|export)?\s*type\s+([A-Za-z_]\w*)\s*=/;
export const HOOK_RE         = /^\s*hook\s+([A-Za-z_]\w*)/;
export const LET_RE          = /^\s*(?:let|const|constexpr|static|int|string|str|bool|char|auto|double|float|array|map|dictionary|hashmap|Array<[^>]+>|Map<[^>]+>|HashMap<[^>]+>)\s+([A-Za-z_]\w*)/;
export const GLOBAL_RE       = /^\s*(?:public|private|export)?\s*global\s+([A-Za-z_]\w*)/;
export const INCLUDE_ALIAS_RE = /^\s*#\s*include\s*(<[^>]+>|"[^"]+"|[^\s;]+)\s*(?:as\s+([A-Za-z_]\w*))?\s*;?\s*$/;
// Matches: import <path> as alias  |  import "path" as alias  |  import 'path' as alias  |  import bareident as alias
export const IMPORT_ALIAS_RE  = /^\s*import\s+(?:<([^>]+)>|"([^"]+)"|'([^']+)'|([A-Za-z_][\w./-]*))\s*(?:as\s+([A-Za-z_]\w*))?/;

// ─── Method / Keyword Lists ─────────────────────────────────────────────────

const FS_METHODS    = ['read','write','append','exists','mkdir','copy','move','remove','list','cwd','chdir','join','parent','dirname','name','basename','ext'];
const PATH_METHODS  = ['join','parent','dirname','name','basename','ext','exists'];

export const MODULE_METHODS: Record<string, string[]> = {
  'builtin/fs':       FS_METHODS,
  'builtin/erefs':    FS_METHODS,
  'builtin/path':     PATH_METHODS,
  'builtin/erepath':  PATH_METHODS,
  'builtin/regex':    ['match','find','replace'],
  'builtin/crypto':   ['hash','random_bytes'],
  'builtin/network':  ['get','download','encode','http_get','http_download','hls_download_best','url_encode'],
  'builtin/net':      ['get','download','encode','http_get','http_download','hls_download_best','url_encode'],
  'builtin/binary':   ['bin_new','bin_from_hex','bin_to_hex','bin_len','bin_get','bin_set','bin_fill','bin_slice'],
  'builtin/threads':  ['thread_run','thread_join','thread_join_timeout','thread_done','thread_list','thread_wait_all','thread_count','thread_yield','thread_gc','thread_gc_all','thread_purge','thread_remove','thread_state'],
  'builtin/monitor':  ['monitor_add','monitor_remove','monitor_list','monitor_info','monitor_last_change','monitor_set_interval'],
  'builtin/math':     ['add','sub','mul','div','mod','min','max','abs','sin','cos','tan','sqrt','pow','collatz_len','collatz_sweep','collatz_best_n','collatz_best_steps','collatz_total_steps','collatz_avg_steps'],
  'builtin/data':     ['data_new','data_set','data_get','data_has','data_keys','data_save','data_load'],
  'builtin/perm':     ['perm_grant','perm_revoke','perm_has','perm_list'],
  'builtin/system':   ['cmd','execute','output','last_exit'],
};

export const CHAIN_METHODS     = ['lstrip','rstrip','strip','lower','upper'];

export const ARRAY_METHODS     = [
  'forEach','push','push_back','emplace_back','append','insert','set','get','at',
  'erase','remove_at','remove','pop','pop_back','front','back','first','last',
  'len','size','length','capacity','empty','clear','contains','find','index_of',
  'join','reserve','shrink_to_fit',
];

export const DICTIONARY_METHODS = [
  'set','put','insert','emplace','try_emplace','insert_or_assign','get','at',
  'has','contains','containsKey','count','getOr','getOrDefault','get_or',
  'get_or_default','remove','erase','clear','size','len','length','empty',
  'keys','values','items','entries','merge','clone',
  'set_path','get_path','has_path','remove_path','forEach',
];

export const LANGUAGE_KEYWORDS = [
  'entity','action','field','let','const','global','int','double','string',
  'bool','char','auto','Array','Map','HashMap','constexpr','static','struct',
  'enum','type','import','export','run','if','else','for','while','switch','case','default',
  'break','continue','return','match','try','catch','async','await','namespace',
  'unsafe','repeat','do','extern',
  'static_cast','dynamic_cast','reinterpret_cast','bit_cast',
  'sizeof','typeof','decltype','alignof','offsetof','is_base_of',
  '#include','#if','#elif','#else','#endif','#ifdef','#ifndef','#define',
];

export const BUILT_INS: readonly string[] = [
  'print','PRINT','sleep','now_ms','now_iso','env','username','computer_name',
  'machine_guid','uuid','rand_int','hwid','args_count','args_get','input',
  'os.args','os.args_count','os.args_get',
  'toint','toInt','tofloat','tostr','toString',
  'dynamic_cast','reinterpret_cast','bit_cast','bitcast','to_json','from_json',
  'sizeof','typeof','decltype','alignof','offsetof','is_base_of',
  'string.starts_with','string.ends_with','string.find','string.substr','string.len',
  'ptr_new','ptr_get','ptr_set','ptr_free','ptr_valid','malloc','free',
  'realloc','memcpy','memset',
  'make_unique','make_shared','unique_reset','shared_reset',
  'string.lstrip','string.rstrip','string.strip','string.lower','string.upper',
  'read_text','write_text','append_text','file_exists','mkdirs','copy_file',
  'move_file','delete_file','list_files','cwd','chdir',
  'exec','os.exec','spawn','os.spawn','exit','read_line','stdin_read',
  'stderr_print','file_mtime','file_size',
  'option_none','option_some','option_is_some','option_unwrap_or',
  'option.none','option.some','option.is_some','option.unwrap_or',
  'result_ok','result_err','result_is_ok','result_unwrap_or',
  'result.ok','result.err','result.is_ok','result.unwrap_or',
  'file_open','file_close','file_read','file_write','file_seek','file_tell',
  'file_flush','fopen','fclose','fread','fwrite','fseek','ftell','fflush',
  'path_join','path_dirname','path_basename','path_ext',
  'strbuf_new','strbuf_append','strbuf_clear','strbuf_len',
  'strbuf_to_string','strbuf_free','strbuf_reserve',
  'string_buffer_new','string_buffer_append','string_buffer_clear',
  'string_buffer_len','string_buffer_to_string','string_buffer_free',
  'string_buffer_reserve',
  'color.red','color.green','color.yellow','color.blue','color.magenta',
  'color.cyan','color.bold','color.reset',
  'set_new','set_add','set_has','set_remove','set_size','set_values',
  'set_union','set_intersect','set_diff',
  'queue_new','queue_push','queue_pop','queue_peek','queue_len','queue_clear',
  'table_new','table_put','table_get','table_has','table_remove','table_rows',
  'table_columns','table_row_keys','table_clear_row','table_count_row',
  'http_get','http_download','hls_download_best','url_encode',
  'network.ip.flush','network.ip.release','network.ip.renew',
  'network.ip.registerdns',
  'network.debug.enable','network.debug.disable','network.debug.status',
  'network.debug.last','network.debug.clear','network.debug.log_tail',
  'language_name','language_version','language_about','language_limitations',
  'win_window_create','win_button_create','win_checkbox_create',
  'win_radiobutton_create','win_slider_create','win_textbox_create',
  'win_label_create','win_on','win_show','win_loop','win_get_text',
  'win_set_text','win_get_check','win_set_check','win_get_slider',
  'win_set_slider','win_close','win_auto_scale','win_set_scale',
  'win_message_box',
  'ui_window_create','ui_label','ui_button','ui_checkbox','ui_radio',
  'ui_slider','ui_textbox','ui_same_line','ui_newline','ui_spacer',
  'ui_separator','ui_load',
  'data_new','data_set','data_get','data_has','data_keys','data_save','data_load',
  'hash_fnv1a','random_bytes',
  'regex_match','regex_find','regex_replace',
  'perm_grant','perm_revoke','perm_has','perm_list',
  'bin_new','bin_from_hex','bin_to_hex','bin_len','bin_get','bin_set',
  'bin_fill','bin_slice',
  'thread_run','thread_join','thread_done',
  'collatz_len','collatz_sweep','collatz_best_steps','collatz_avg_steps',
  'char_is_digit','char_is_space','char_is_alpha','char_is_ident_start',
  'char_is_ident_part',
];

export const DEPRECATED_BUILT_INS = new Set<string>([
  'list_new','list_push','list_get','list_len','list_join','list_clear',
  'list_remove_at',
  'dict_new','dict_set','dict_get','dict_has','dict_keys','dict_values',
  'dict_get_or','dict_remove','dict_clear','dict_size','dict_merge',
  'dict_clone','dict_items','dict_entries',
  'dict_set_path','dict_get_path','dict_has_path','dict_remove_path',
  'hashmap_new','hashmap_set','hashmap_put','hashmap_get','hashmap_has',
  'hashmap_contains','hashmap_get_or','hashmap_get_or_default',
  'hashmap_remove','hashmap_clear','hashmap_size','hashmap_keys',
  'hashmap_values','hashmap_merge',
]);
