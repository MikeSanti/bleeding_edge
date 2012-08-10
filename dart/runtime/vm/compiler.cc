// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler.h"

#include "vm/assembler.h"
#include "vm/ast_printer.h"
#include "vm/code_generator.h"
#include "vm/code_patcher.h"
#include "vm/dart_entry.h"
#include "vm/debugger.h"
#include "vm/disassembler.h"
#include "vm/exceptions.h"
#include "vm/flags.h"
#include "vm/flow_graph_allocator.h"
#include "vm/flow_graph_builder.h"
#include "vm/flow_graph_compiler.h"
#include "vm/flow_graph_optimizer.h"
#include "vm/il_printer.h"
#include "vm/longjump.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/os.h"
#include "vm/parser.h"
#include "vm/scanner.h"
#include "vm/symbols.h"
#include "vm/timer.h"

namespace dart {

DEFINE_FLAG(bool, disassemble, false, "Disassemble dart code.");
DEFINE_FLAG(bool, disassemble_optimized, false, "Disassemble optimized code.");
DEFINE_FLAG(bool, trace_bailout, false, "Print bailout from ssa compiler.");
DEFINE_FLAG(bool, trace_compiler, false, "Trace compiler operations.");
DEFINE_FLAG(bool, use_ssa, true, "Use SSA form");
DEFINE_FLAG(int, deoptimization_counter_threshold, 5,
    "How many times we allow deoptimization before we disallow"
    " certain optimizations");
DECLARE_FLAG(bool, print_flow_graph);


// Compile a function. Should call only if the function has not been compiled.
//   Arg0: function object.
DEFINE_RUNTIME_ENTRY(CompileFunction, 1) {
  ASSERT(arguments.Count() == kCompileFunctionRuntimeEntry.argument_count());
  const Function& function = Function::CheckedHandle(arguments.At(0));
  ASSERT(!function.HasCode());
  const Error& error = Error::Handle(Compiler::CompileFunction(function));
  if (!error.IsNull()) {
    Exceptions::PropagateError(error);
  }
}


// Returns an array indexed by deopt id, containing the extracted ICData.
static RawArray* ExtractTypeFeedbackArray(const Code& code) {
  ASSERT(!code.IsNull() && !code.is_optimized());
  GrowableArray<intptr_t> deopt_ids;
  const GrowableObjectArray& ic_data_objs =
      GrowableObjectArray::Handle(GrowableObjectArray::New());
  const intptr_t max_id =
      code.ExtractIcDataArraysAtCalls(&deopt_ids, ic_data_objs);
  const Array& result = Array::Handle(Array::New(max_id + 1));
  for (intptr_t i = 0; i < deopt_ids.length(); i++) {
    intptr_t result_index = deopt_ids[i];
    ASSERT(result.At(result_index) == Object::null());
    result.SetAt(result_index, Object::Handle(ic_data_objs.At(i)));
  }
  return result.raw();
}


RawError* Compiler::Compile(const Library& library, const Script& script) {
  Isolate* isolate = Isolate::Current();
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  if (setjmp(*jump.Set()) == 0) {
    if (FLAG_trace_compiler) {
      HANDLESCOPE(isolate);
      const String& script_url = String::Handle(script.url());
      // TODO(iposva): Extract script kind.
      OS::Print("Compiling %s '%s'\n", "", script_url.ToCString());
    }
    const String& library_key = String::Handle(library.private_key());
    script.Tokenize(library_key);
    Parser::ParseCompilationUnit(library, script);
    isolate->set_long_jump_base(base);
    return Error::null();
  } else {
    Error& error = Error::Handle();
    error = isolate->object_store()->sticky_error();
    isolate->object_store()->clear_sticky_error();
    isolate->set_long_jump_base(base);
    return error.raw();
  }
  UNREACHABLE();
  return Error::null();
}


static void InstallUnoptimizedCode(const Function& function) {
  // Disable optimized code.
  ASSERT(function.HasOptimizedCode());
  // Patch entry of optimized code.
  CodePatcher::PatchEntry(Code::Handle(function.CurrentCode()));
  if (FLAG_trace_compiler) {
    OS::Print("--> patching entry 0x%x\n",
              Code::Handle(function.CurrentCode()).EntryPoint());
  }
  // Use previously compiled code.
  function.SetCode(Code::Handle(function.unoptimized_code()));
  CodePatcher::RestoreEntry(Code::Handle(function.unoptimized_code()));
  if (FLAG_trace_compiler) {
    OS::Print("--> restoring entry at 0x%x\n",
              Code::Handle(function.unoptimized_code()).EntryPoint());
  }
}


// Return false if bailed out.
static bool CompileParsedFunctionHelper(
    const ParsedFunction& parsed_function, bool optimized, bool use_ssa) {
  TimerScope timer(FLAG_compiler_stats, &CompilerStats::codegen_timer);
  bool is_compiled = false;
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate->ic_data_array() == Array::null());  // Must be reset to null.
  const intptr_t prev_deopt_id = isolate->deopt_id();
  isolate->set_deopt_id(0);
  LongJump* old_base = isolate->long_jump_base();
  LongJump bailout_jump;
  isolate->set_long_jump_base(&bailout_jump);
  if (setjmp(*bailout_jump.Set()) == 0) {
    GrowableArray<BlockEntryInstr*> block_order;
    // TimerScope needs an isolate to be properly terminated in case of a
    // LongJump.
    {
      TimerScope timer(FLAG_compiler_stats,
                       &CompilerStats::graphbuilder_timer,
                       isolate);
      if (optimized) {
        // Transition to optimized code only from unoptimized code ...
        // for now.
        ASSERT(parsed_function.function().HasCode());
        ASSERT(!parsed_function.function().HasOptimizedCode());
        // Extract type feedback before the graph is built, as the graph
        // builder uses it to attach it to nodes.
        // Do not use type feedback to optimize a function that was
        // deoptimized too often.
        if (parsed_function.function().deoptimization_counter() <
            FLAG_deoptimization_counter_threshold) {
          const Code& unoptimized_code =
              Code::Handle(parsed_function.function().unoptimized_code());
          isolate->set_ic_data_array(
              ExtractTypeFeedbackArray(unoptimized_code));
        }
      }
      FlowGraphBuilder graph_builder(parsed_function);
      graph_builder.BuildGraph(optimized, use_ssa);

      // The non-optimizing compiler compiles blocks in reverse postorder,
      // because it is a 'natural' order for the human reader of the
      // generated code.
      intptr_t length = graph_builder.postorder_block_entries().length();
      for (intptr_t i = length - 1; i >= 0; --i) {
        block_order.Add(graph_builder.postorder_block_entries()[i]);
      }
      if (optimized) {
        FlowGraphOptimizer optimizer(block_order);
        optimizer.ApplyICData();

        // Propagate types and eliminate more type tests.
        FlowGraphTypePropagator propagator(parsed_function, block_order);
        propagator.PropagateTypes();

        if (use_ssa) {
          // Perform register allocation on the SSA graph.
          FlowGraphAllocator allocator(block_order, &graph_builder);
          allocator.AllocateRegisters();
        }
        if (FLAG_print_flow_graph) {
          OS::Print("After Optimizations:\n");
          FlowGraphPrinter printer(Function::Handle(), block_order);
          printer.PrintBlocks();
        }
      }
    }

    bool is_leaf = false;
    if (optimized) {
      FlowGraphAnalyzer analyzer(block_order);
      analyzer.Analyze();
      is_leaf = analyzer.is_leaf();
    }
    Assembler assembler;
    FlowGraphCompiler graph_compiler(&assembler,
                                     parsed_function,
                                     block_order,
                                     optimized,
                                     optimized && use_ssa,
                                     is_leaf);
    {
      TimerScope timer(FLAG_compiler_stats,
                       &CompilerStats::graphcompiler_timer,
                       isolate);
      graph_compiler.CompileGraph();
    }
    {
      TimerScope timer(FLAG_compiler_stats,
                       &CompilerStats::codefinalizer_timer,
                       isolate);
      const Function& function = parsed_function.function();
      const Code& code = Code::Handle(Code::FinalizeCode(function, &assembler));
      code.set_is_optimized(optimized);
      graph_compiler.FinalizePcDescriptors(code);
      graph_compiler.FinalizeDeoptInfo(code);
      graph_compiler.FinalizeStackmaps(code);
      graph_compiler.FinalizeVarDescriptors(code);
      graph_compiler.FinalizeExceptionHandlers(code);
      graph_compiler.FinalizeComments(code);
      if (optimized) {
        function.SetCode(code);
        CodePatcher::PatchEntry(Code::Handle(function.unoptimized_code()));
        if (FLAG_trace_compiler) {
          OS::Print("--> patching entry 0x%x\n",
                    Code::Handle(function.unoptimized_code()).EntryPoint());
        }
      } else {
        function.set_unoptimized_code(code);
        function.SetCode(code);
        ASSERT(CodePatcher::CodeIsPatchable(code));
      }
    }
    is_compiled = true;
  } else {
    // We bailed out.
    Error& bailout_error = Error::Handle(
        isolate->object_store()->sticky_error());
    isolate->object_store()->clear_sticky_error();
    if (FLAG_trace_bailout) {
      OS::Print("%s\n", bailout_error.ToErrorCString());
    }
    // We only bail out from generating ssa code.
    ASSERT(optimized && use_ssa);
    is_compiled = false;
  }
  // Reset global isolate state.
  isolate->set_ic_data_array(Array::null());
  isolate->set_long_jump_base(old_base);
  isolate->set_deopt_id(prev_deopt_id);
  return is_compiled;
}


static void DisassembleCode(const Function& function, bool optimized) {
  const char* function_fullname = function.ToFullyQualifiedCString();
  OS::Print("Code for %sfunction '%s' {\n",
            optimized ? "optimized " : "",
            function_fullname);
  const Code& code = Code::Handle(function.CurrentCode());
  const Instructions& instructions =
      Instructions::Handle(code.instructions());
  uword start = instructions.EntryPoint();
  Disassembler::Disassemble(start,
                            start + instructions.size(),
                            code.comments());
  OS::Print("}\n");
  OS::Print("Pointer offsets for function: {\n");
  // Pointer offsets are stored in descending order.
  for (intptr_t i = code.pointer_offsets_length() - 1; i >= 0; i--) {
    const uword addr = code.GetPointerOffsetAt(i) + code.EntryPoint();
    Object& obj = Object::Handle();
    obj = *reinterpret_cast<RawObject**>(addr);
    OS::Print(" %" PRIdPTR " : 0x%" PRIxPTR " '%s'\n",
              code.GetPointerOffsetAt(i), addr, obj.ToCString());
  }
  OS::Print("}\n");
  OS::Print("PC Descriptors for function '%s' {\n", function_fullname);
  OS::Print("(pc\t\tkind\t\tid\ttok-ix\ttry/deopt-ix)\n");
  const PcDescriptors& descriptors =
      PcDescriptors::Handle(code.pc_descriptors());
  OS::Print("%s\n", descriptors.ToCString());
  OS::Print("}\n");
  const Array& deopt_info_array = Array::Handle(code.deopt_info_array());
  if (deopt_info_array.Length() > 0) {
    OS::Print("DeoptInfo: {\n");
    for (intptr_t i = 0; i < deopt_info_array.Length(); i++) {
      OS::Print("  %d: %s\n",
          i, Object::Handle(deopt_info_array.At(i)).ToCString());
    }
    OS::Print("}\n");
  }
  const Array& object_table = Array::Handle(code.object_table());
  if (object_table.Length() > 0) {
    OS::Print("Object Table: {\n");
    for (intptr_t i = 0; i < object_table.Length(); i++) {
      OS::Print("  %d: %s\n", i,
          Object::Handle(object_table.At(i)).ToCString());
    }
    OS::Print("}\n");
  }
  OS::Print("Variable Descriptors for function '%s' {\n",
            function_fullname);
  const LocalVarDescriptors& var_descriptors =
      LocalVarDescriptors::Handle(code.var_descriptors());
  intptr_t var_desc_length =
      var_descriptors.IsNull() ? 0 : var_descriptors.Length();
  String& var_name = String::Handle();
  for (intptr_t i = 0; i < var_desc_length; i++) {
    var_name = var_descriptors.GetName(i);
    RawLocalVarDescriptors::VarInfo var_info;
    var_descriptors.GetInfo(i, &var_info);
    if (var_info.kind == RawLocalVarDescriptors::kContextChain) {
      OS::Print("  saved CTX reg offset %" PRIdPTR "\n", var_info.index);
    } else {
      if (var_info.kind == RawLocalVarDescriptors::kContextLevel) {
        OS::Print("  context level %" PRIdPTR " scope %d",
                  var_info.index, var_info.scope_id);
      } else if (var_info.kind == RawLocalVarDescriptors::kStackVar) {
        OS::Print("  stack var '%s' offset %" PRIdPTR,
                  var_name.ToCString(), var_info.index);
      } else {
        ASSERT(var_info.kind == RawLocalVarDescriptors::kContextVar);
        OS::Print("  context var '%s' level %d offset %" PRIdPTR,
                  var_name.ToCString(), var_info.scope_id, var_info.index);
      }
      OS::Print(" (valid %" PRIdPTR "-%" PRIdPTR ")\n",
                var_info.begin_pos, var_info.end_pos);
    }
  }
  OS::Print("}\n");
  OS::Print("Exception Handlers for function '%s' {\n", function_fullname);
  const ExceptionHandlers& handlers =
        ExceptionHandlers::Handle(code.exception_handlers());
  OS::Print("%s\n", handlers.ToCString());
  OS::Print("}\n");
}


static RawError* CompileFunctionHelper(const Function& function,
                                       bool optimized) {
  Isolate* isolate = Isolate::Current();
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  // Skips parsing if we need to only install unoptimized code.
  if (!optimized && !Code::Handle(function.unoptimized_code()).IsNull()) {
    InstallUnoptimizedCode(function);
    isolate->set_long_jump_base(base);
    return Error::null();
  }
  if (setjmp(*jump.Set()) == 0) {
    TIMERSCOPE(time_compilation);
    ParsedFunction parsed_function(function);
    if (FLAG_trace_compiler) {
      OS::Print("Compiling %sfunction: '%s' @ token %d\n",
                (optimized ? "optimized " : ""),
                function.ToFullyQualifiedCString(),
                function.token_pos());
    }
    Parser::ParseFunction(&parsed_function);
    parsed_function.AllocateVariables();

    if (!CompileParsedFunctionHelper(parsed_function,
                                     optimized,
                                     FLAG_use_ssa)) {
      // Compile again using non-ssa code generation.
      // Re-parse because of side-effects to the AST during compilation.
      ParsedFunction parsed_function(function);
      Parser::ParseFunction(&parsed_function);
      parsed_function.AllocateVariables();
      CompileParsedFunctionHelper(parsed_function, optimized, false);
    }

    if (FLAG_trace_compiler) {
      OS::Print("--> '%s' entry: 0x%x\n",
                function.ToFullyQualifiedCString(),
                Code::Handle(function.CurrentCode()).EntryPoint());
    }
    if (Isolate::Current()->debugger()->IsActive()) {
      Isolate::Current()->debugger()->NotifyCompilation(function);
    }
    if (FLAG_disassemble) {
      DisassembleCode(function, optimized);
    }
    if (FLAG_disassemble_optimized && optimized) {
      // Print unoptimized code along with the optimized code.
      DisassembleCode(function, false);
      DisassembleCode(function, true);
    }
    isolate->set_long_jump_base(base);
    return Error::null();
  } else {
    Error& error = Error::Handle();
    // We got an error during compilation.
    error = isolate->object_store()->sticky_error();
    isolate->object_store()->clear_sticky_error();
    isolate->set_long_jump_base(base);
    return error.raw();
  }
  UNREACHABLE();
  return Error::null();
}


RawError* Compiler::CompileFunction(const Function& function) {
  return CompileFunctionHelper(function, false);  // Non-optimized.
}


RawError* Compiler::CompileOptimizedFunction(const Function& function) {
  return CompileFunctionHelper(function, true);  // Optimized.
}


RawError* Compiler::CompileParsedFunction(
    const ParsedFunction& parsed_function) {
  Isolate* isolate = Isolate::Current();
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  if (setjmp(*jump.Set()) == 0) {
    // Non-optimized, non-ssa code generator.
    CompileParsedFunctionHelper(parsed_function, false, false);
    isolate->set_long_jump_base(base);
    return Error::null();
  } else {
    Error& error = Error::Handle();
    // We got an error during compilation.
    error = isolate->object_store()->sticky_error();
    isolate->object_store()->clear_sticky_error();
    isolate->set_long_jump_base(base);
    return error.raw();
  }
  UNREACHABLE();
  return Error::null();
}


RawError* Compiler::CompileAllFunctions(const Class& cls) {
  Error& error = Error::Handle();
  Array& functions = Array::Handle(cls.functions());
  Function& func = Function::Handle();
  for (int i = 0; i < functions.Length(); i++) {
    func ^= functions.At(i);
    ASSERT(!func.IsNull());
    if (!func.HasCode() && !func.is_abstract()) {
      error = CompileFunction(func);
      if (!error.IsNull()) {
        return error.raw();
      }
    }
  }
  return error.raw();
}


RawObject* Compiler::ExecuteOnce(SequenceNode* fragment) {
  Isolate* isolate = Isolate::Current();
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  if (setjmp(*jump.Set()) == 0) {
    if (FLAG_trace_compiler) {
      OS::Print("compiling expression: ");
      AstPrinter::PrintNode(fragment);
    }

    // Create a dummy function object for the code generator.
    // The function needs to be associated with a named Class: the interface
    // Function fits the bill.
    const char* kEvalConst = "eval_const";
    const Function& func = Function::Handle(Function::New(
        String::Handle(Symbols::New(kEvalConst)),
        RawFunction::kConstImplicitGetter,
        true,  // static function.
        false,  // not const function.
        false,  // not abstract
        false,  // not external.
        Class::Handle(Type::Handle(Type::FunctionInterface()).type_class()),
        fragment->token_pos()));

    func.set_result_type(Type::Handle(Type::DynamicType()));
    func.set_num_fixed_parameters(0);
    func.set_num_optional_parameters(0);

    // We compile the function here, even though InvokeStatic() below
    // would compile func automatically. We are checking fewer invariants
    // here.
    ParsedFunction parsed_function(func);
    parsed_function.SetNodeSequence(fragment);
    parsed_function.set_default_parameter_values(Array::Handle());
    parsed_function.set_expression_temp_var(
        ParsedFunction::CreateExpressionTempVar(0));
    fragment->scope()->AddVariable(parsed_function.expression_temp_var());
    parsed_function.AllocateVariables();

    // Non-optimized, non-ssa code generator.
    CompileParsedFunctionHelper(parsed_function, false, false);

    GrowableArray<const Object*> arguments;  // no arguments.
    const Array& kNoArgumentNames = Array::Handle();
    Object& result = Object::Handle();
    result = DartEntry::InvokeStatic(func,
                                     arguments,
                                     kNoArgumentNames);
    isolate->set_long_jump_base(base);
    return result.raw();
  } else {
    Object& result = Object::Handle();
    result = isolate->object_store()->sticky_error();
    isolate->object_store()->clear_sticky_error();
    isolate->set_long_jump_base(base);
    return result.raw();
  }
  UNREACHABLE();
  return Object::null();
}

}  // namespace dart
