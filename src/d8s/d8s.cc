/*
 * 模仿 d8 写一个调试 shell, 用于学习 v8
 */

#include "src/api/api-inl.h"
// #include "src/base/logging.h"
#include "src/base/platform/platform.h"
// #include "src/base/platform/time.h"
#include "include/libplatform/libplatform.h"
#include "src/base/sys-info.h"
#include "src/init/v8.h"
#include "src/execution/vm-state-inl.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"

using namespace v8;

int RunMain(Isolate* isolate, bool last_run);
Local<Context> CreateEvaluationContext(Isolate* isolate);
bool Execute(Isolate* isolate);
Local<String> ReadFile(Isolate* isolate, const char* name);
bool ExecuteString(Isolate* isolate, Local<String> source,
                          Local<Value> name, bool print_result,
                          bool report_exceptions,
                          bool process_message_queue);

int main(int argc, char* argv[]) {
  v8::base::EnsureConsoleOutput();
  // 交互式 shell

  // v8::V8::InitializeICUDefaultLocation(argv[0], options.icu_data_file);

  std::unique_ptr<v8::Platform> platform = platform::NewDefaultPlatform(16);
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  // InitializeExternalStartupDataFromFile: snapshot_blob
  // v8::V8::InitializeExternalStartupDataFromFile(options.snapshot_blob);
  v8::V8::InitializeExternalStartupData(argv[0]);

  Isolate::CreateParams create_params;

  create_params.constraints.ConfigureDefaults(
      base::SysInfo::AmountOfPhysicalMemory(),
      base::SysInfo::AmountOfVirtualMemory());
  create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  Isolate* isolate = Isolate::New(create_params);

  // console
  int result = 0;
  bool last_run = true;
  result = RunMain(isolate, last_run);

  V8::Dispose();
  V8::ShutdownPlatform();

  printf("quit d8s\n");
}

int RunMain(Isolate* isolate, bool last_run) {
  printf("RunMain\n");

  HandleScope scope(isolate);
  Local<Context> context = CreateEvaluationContext(isolate);
  Context::Scope cscope(context);
  // InspectorClient inspector_client(context, options.enable_inspector);
  // PerIsolateData::RealmScope realm_scope(PerIsolateData::Get(isolate));
  bool succ = Execute(isolate);
  printf("RunMain succ=%d\n", succ);
  return 0;
}

// 全局变量
base::LazyMutex context_mutex_;
Local<Context> CreateEvaluationContext(Isolate* isolate) {
  // This needs to be a critical section since this is not thread-safe
  base::MutexGuard lock_guard(context_mutex_.Pointer());
  // Initialize the global objects
  // Local<ObjectTemplate> global_template = CreateGlobalTemplate(isolate);
  EscapableHandleScope handle_scope(isolate);
  Local<Context> context = Context::New(isolate, nullptr); //, global_template);
  DCHECK(!context.IsEmpty());
  if (i::FLAG_perf_prof_annotate_wasm || i::FLAG_vtune_prof_annotate_wasm) {
    isolate->SetWasmLoadSourceMapCallback(ReadFile);
  }
  // InitializeModuleEmbedderData(context);
  return handle_scope.Escape(context);
}

bool Execute(Isolate* isolate) {
    // Use all other arguments as names of files to load and run.
    HandleScope handle_scope(isolate);
    char arg[] = "./test.js";
    Local<String> file_name =
        String::NewFromUtf8(isolate, arg).ToLocalChecked();
    Local<String> source = ReadFile(isolate, arg);
    if (source.IsEmpty()) {
      printf("Error reading '%s'\n", arg);
      base::OS::ExitProcess(1);
    }
  
    return ExecuteString(isolate, source, file_name, false, true, false);
}

Local<String> ReadFile(Isolate* isolate, const char* name) {
  std::unique_ptr<base::OS::MemoryMappedFile> file(
      base::OS::MemoryMappedFile::open(
          name, base::OS::MemoryMappedFile::FileMode::kReadOnly));
  if (!file) return Local<String>();

  int size = static_cast<int>(file->size());
  char* chars = static_cast<char*>(file->memory());
  Local<String> result;
  result = String::NewFromUtf8(isolate, chars, NewStringType::kNormal, size)
                .ToLocalChecked();
  return result;
}

bool ExecuteString(Isolate* isolate, Local<String> source,
                          Local<Value> name, bool print_result,
                          bool report_exceptions,
                          bool process_message_queue) {
  if (i::FLAG_parse_only) {
    i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
    i::VMState<PARSER> state(i_isolate);
    // Local<String> 转 v8::internal::Handle<String>
    // 反过来是不是就可以实现之前的问题了呢? refer to api.h
    i::Handle<i::String> str = Utils::OpenHandle(*(source));
    
    v8::String::Utf8Value v8Src(isolate, source);
    fprintf(stderr, "source=%u[%s]\n", v8Src.length(), *v8Src);

    // Set up ParseInfo.
    i::UnoptimizedCompileState compile_state(i_isolate);

    i::UnoptimizedCompileFlags flags =
        i::UnoptimizedCompileFlags::ForToplevelCompile(
            i_isolate, true, i::construct_language_mode(i::FLAG_use_strict),
            i::REPLMode::kNo);

    i::ParseInfo parse_info(i_isolate, flags, &compile_state);

    // 这里开始解析代码, 结果是 Script 对象
    i::Handle<i::Script> script = parse_info.CreateScript(
        i_isolate, str, i::kNullMaybeHandle, ScriptOriginOptions());
    if (!i::parsing::ParseProgram(&parse_info, script, i_isolate,
                                  i::parsing::ReportStatisticsMode::kYes)) {
      parse_info.pending_error_handler()->PrepareErrors(
          i_isolate, parse_info.ast_value_factory());
      parse_info.pending_error_handler()->ReportErrors(i_isolate, script);

      fprintf(stderr, "Failed parsing\n");
      return false;
    }
    return true;
  }

  HandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);
  try_catch.SetVerbose(report_exceptions == true);

  MaybeLocal<Value> maybe_result;
  bool success = true;
  {
    // PerIsolateData* data = PerIsolateData::Get(isolate);
    Local<Context> realm =
        Local<Context>::New(isolate, nullptr); //data->realms_[data->realm_current_]);
    Context::Scope context_scope(realm);
    MaybeLocal<Script> maybe_script;
    Local<Context> context(isolate->GetCurrentContext());
    ScriptOrigin origin(name);  // name 是脚本名字, source 是脚本内容

    ScriptCompiler::Source script_source(source, origin);
    maybe_script = ScriptCompiler::Compile(context, &script_source,
                                           v8::ScriptCompiler::kNoCompileOptions);

    Local<Script> script;
    if (!maybe_script.ToLocal(&script)) {
      return false;
    }

    maybe_result = script->Run(realm);
    // data->realm_current_ = data->realm_switch_;
  }
  Local<Value> result;
  if (!maybe_result.ToLocal(&result)) {
    DCHECK(try_catch.HasCaught());
    return false;
  }
  // It's possible that a FinalizationRegistry cleanup task threw an error.
  if (try_catch.HasCaught()) success = false;
  {
      v8::String::Utf8Value str(isolate, v8::JSON::Stringify(isolate, result));
      fwrite(*str, sizeof(**str), str.length(), stdout);
      printf("\n");
  }
  return success;
}
