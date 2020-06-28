/*
 * fn_ir_builder.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "codegen/fn_ir_builder.h"
#include <stack>
#include <string>
#include "codegen/block_ir_builder.h"
#include "codegen/ir_base_builder.h"
#include "codegen/type_ir_builder.h"
#include "codegen/variable_ir_builder.h"
#include "glog/logging.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"

namespace fesql {
namespace codegen {

FnIRBuilder::FnIRBuilder(::llvm::Module *module) : module_(module) {}

FnIRBuilder::~FnIRBuilder() {}

bool FnIRBuilder::Build(const ::fesql::node::FnNodeFnDef *root,
                        ::llvm::Function** result,
                        base::Status &status) {  // NOLINT
    if (root == NULL || root->GetType() != ::fesql::node::kFnDef) {
        status.code = common::kCodegenError;
        status.msg = "node is null";
        LOG(WARNING) << status.msg;
        return false;
    }
    ::llvm::Function *fn = NULL;
    ::llvm::BasicBlock *block = NULL;
    std::stack<int32_t> indent_stack;
    ScopeVar sv;
    sv.Enter("module");

    const ::fesql::node::FnNodeFnHeander *fn_def = root->header_;

    bool ok = BuildFnHead(fn_def, &sv, &fn, status);
    if (!ok) {
        return false;
    }
    block = ::llvm::BasicBlock::Create(module_->getContext(), "entry", fn);
    llvm::BasicBlock *end_block =
        llvm::BasicBlock::Create(module_->getContext(), "end_block");

    BlockIRBuilder block_ir_builder(&sv);
    if (false ==
        block_ir_builder.BuildBlock(root->block_, block, end_block, status)) {
        return false;
    }
    *result = fn;
    return true;
}

bool FnIRBuilder::BuildFnHead(const ::fesql::node::FnNodeFnHeander *header,
                              ScopeVar *sv, ::llvm::Function **fn,
                              base::Status &status) {  // NOLINE
    ::llvm::Type *ret_type = NULL;
    bool ok = GetLLVMType(module_, header->ret_type_, &ret_type);
    if (!ok) {
        status.code = common::kCodegenError;
        status.msg = "fail to get llvm type";
        return false;
    }

    if (TypeIRBuilder::IsStructPtr(ret_type)) {
        return BuildFnHeadWithRetStruct(header, sv, fn, status);
    }

    if (!CreateFunction(header, fn, status)) {
        LOG(WARNING) << "Fail Build Function Header: " << status.msg;
        return false;
    }
    auto fn_name = sv->Enter((*fn)->getName().str());
    if (header->parameters_) {
        bool ok = FillArgs(header->parameters_, sv, *fn, status);
        if (!ok) {
            return false;
        }
    }
    DLOG(INFO) << "build fn " << fn_name << " header done";
    return true;
}
bool FnIRBuilder::BuildFnHeadWithRetStruct(
    const ::fesql::node::FnNodeFnHeander *fn_def, ScopeVar *sv,
    ::llvm::Function **fn,
    base::Status &status) {  // NOLINE
    if (fn_def == NULL || fn == NULL) {
        status.code = common::kCodegenError;
        status.msg = "input is null";
        LOG(WARNING) << status.msg;
        return false;
    }
    node::FnNodeList new_parameters;
    for_each(fn_def->parameters_->children.cbegin(),
             fn_def->parameters_->children.cend(),
             [&](node::FnNode *node) { new_parameters.AddChild(node); });
    node::FnParaNode ret_arg("@ret_struct", fn_def->ret_type_);
    new_parameters.AddChild(&ret_arg);
    node::TypeNode ret(node::kBool);
    node::FnNodeFnHeander header(fn_def->name_, &new_parameters, &ret);
    return BuildFnHead(&header, sv, fn, status);
}
bool FnIRBuilder::CreateFunction(const ::fesql::node::FnNodeFnHeander *fn_def,
                                 ::llvm::Function **fn,
                                 base::Status &status) {  // NOLINE
    if (fn_def == NULL || fn == NULL) {
        status.code = common::kCodegenError;
        status.msg = "input is null";
        LOG(WARNING) << status.msg;
        return false;
    }

    ::llvm::Type *ret_type = NULL;
    bool ok = GetLLVMType(module_, fn_def->ret_type_, &ret_type);
    if (!ok) {
        status.code = common::kCodegenError;
        status.msg = "fail to get llvm type";
        return false;
    }
    std::vector<::llvm::Type *> paras;
    if (nullptr != fn_def->parameters_) {
        bool ok = BuildParas(fn_def->parameters_, paras, status);
        if (!ok) {
            return false;
        }
    }

    std::string fn_name = fn_def->GeIRFunctionName();
    ::llvm::ArrayRef<::llvm::Type *> array_ref(paras);
    ::llvm::FunctionType *fnt =
        ::llvm::FunctionType::get(ret_type, array_ref, false);
    auto callee = module_->getOrInsertFunction(fn_name, fnt);
    *fn = reinterpret_cast<::llvm::Function *>(callee.getCallee());
    return true;
}

bool FnIRBuilder::FillArgs(const ::fesql::node::FnNodeList *node, ScopeVar *sv,
                           ::llvm::Function *fn,
                           base::Status &status) {  // NOLINE
    if (node == NULL) {
        status.code = common::kCodegenError;
        status.msg = "node is null or node type mismatch";
        LOG(WARNING) << status.msg;
        return false;
    }

    ::llvm::Function::arg_iterator it = fn->arg_begin();
    uint32_t index = 0;
    for (; it != fn->arg_end() && index < node->children.size(); ++it) {
        ::fesql::node::FnParaNode *pnode =
            (::fesql::node::FnParaNode *)node->children[index];
        ::llvm::Argument *argu = &*it;

        bool ok = sv->AddVar(pnode->GetName(), NativeValue::Create(argu));
        if (!ok) {
            status.code = common::kCodegenError;
            status.msg = "fail to define var " + pnode->GetName();
            LOG(WARNING) << status.msg;
            return false;
        }
        index++;
    }
    return true;
}

bool FnIRBuilder::BuildParas(const ::fesql::node::FnNodeList *node,
                             std::vector<::llvm::Type *> &paras,
                             base::Status &status) {  // NOLINE
    if (node == NULL) {
        status.code = common::kCodegenError;
        status.msg = "node is null or node type mismatch";
        LOG(WARNING) << status.msg;
        return false;
    }

    for (uint32_t i = 0; i < node->children.size(); i++) {
        ::fesql::node::FnParaNode *pnode =
            (::fesql::node::FnParaNode *)node->children[i];
        ::llvm::Type *type = NULL;
        bool ok = GetLLVMType(module_, pnode->GetParaType(), &type);
        if (!ok) {
            status.code = common::kCodegenError;
            status.msg =
                "fail to get primary type for pname " + pnode->GetName();
            LOG(WARNING) << status.msg;
            return false;
        }
        paras.push_back(type);
    }
    return true;
}

}  // namespace codegen
}  // namespace fesql
