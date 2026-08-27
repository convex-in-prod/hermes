/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"
#include "llvh/ADT/ScopeExit.h"

namespace hermes {
namespace irgen {

void ESTreeIRGen::genClassDeclaration(ESTree::ClassDeclarationNode *node) {
  auto *id = llvh::cast<ESTree::IdentifierNode>(node->_id);
  sema::Decl *decl = getIDDecl(id);
  if (decl->generic) {
    // Skip generics that aren't specialized.
    return;
  }

  flow::Type *declType = flowContext_.findDeclType(decl);
  flow::ClassConstructorType *consType =
      llvh::dyn_cast_or_null<flow::ClassConstructorType>(
          declType ? declType->info : nullptr);

  // If the class is not annotated with a type, it is legacy.
  if (!consType) {
    genLegacyClassDeclaration(node);
    return;
  }

  genTypedClassLike(node, consType, decl, Identifier{});
}

Value *ESTreeIRGen::genClassExpression(
    ESTree::ClassExpressionNode *node,
    Identifier nameHint) {
  auto *consType = llvh::dyn_cast<flow::ClassConstructorType>(
      flowContext_.getNodeTypeOrAny(node)->info);
  if (!consType)
    return genLegacyClassExpression(node, nameHint);

  emitScopeDeclarations(node->getScope());
  sema::Decl *constructorDecl = nullptr;
  if (auto *id = ESTree::getClassID(node))
    constructorDecl = semCtx_.getExpressionDecl(id);
  return genTypedClassLike(node, consType, constructorDecl, nameHint);
}

Value *ESTreeIRGen::genTypedClassLike(
    ESTree::ClassLikeNode *node,
    flow::ClassConstructorType *consType,
    sema::Decl *constructorDecl,
    Identifier nameHint) {
  flow::ClassType *classType = consType->getClassTypeInfo();
  Identifier consName = classType->getClassName().isValid()
      ? classType->getClassName()
      : nameHint;

  Value *superClass = nullptr;
  if (ESTree::getSuperClass(node)) {
    superClass = genExpression(ESTree::getSuperClass(node));
  }

  // Push a new typed class context.
  TypedClassContext savedClsCtx = curFunction()->typedClassContext;
  curFunction()->typedClassContext = {node, classType, consName};
  // Pop the class context on function exit.
  auto popClsContext = llvh::make_scope_exit([&savedClsCtx, this]() {
    curFunction()->typedClassContext = savedClsCtx;
  });

  auto *classBody = ESTree::getClassBody(node);

  // Computed method keys are evaluated in class element order and converted
  // to property keys exactly once. The methods themselves are installed after
  // the constructor and prototype objects have been created.
  llvh::DenseMap<ESTree::MethodDefinitionNode *, Value *> computedMethodKeys;
  bool hasComputedInstanceMethods = false;
  for (ESTree::Node &elem : classBody->_body) {
    auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&elem);
    if (!method || !method->_computed)
      continue;
    if (!method->_static)
      hasComputedInstanceMethods = true;
    computedMethodKeys.try_emplace(
        method, Builder.createToPropertyKeyInst(genExpression(method->_key)));
  }

  // Emit private methods as variables.
  for (auto &[name, idx] :
       classType->getHomeObjectTypeInfo()->getPrivateFieldNameMap()) {
    const flow::ClassType::Field &field =
        classType->getHomeObjectTypeInfo()->getFields()[idx];
    if (field.isMethod() && field.isPrivate) {
      if (field.isOverloaded()) {
        // Overloaded final private method: emit each non-generic overload
        // as its own closure / Variable. Generic overloads are emitted
        // through specializations.
        for (auto &[overloadMethod, overloadType] : field.overloads) {
          if (llvh::isa<flow::GenericType>(overloadType->info))
            continue;
          sema::Decl *decl = semCtx_.getExpressionDecl(
              ESTree::getPropertyIdentifier(overloadMethod->_key));
          emitTypedFinalMethodClosureStore(field, overloadMethod, decl);
        }
        continue;
      }

      // Skip generic private methods - emitted through specializations.
      if (llvh::isa<flow::GenericType>(field.type->info))
        continue;

      if (field.isAccessor()) {
        if (field.method) {
          sema::Decl *decl = semCtx_.getExpressionDecl(
              ESTree::getPropertyIdentifier(field.method->_key));
          emitTypedFinalMethodClosureStore(field, field.method, decl);
        }
        if (field.setterMethod) {
          sema::Decl *decl = semCtx_.getExpressionDecl(
              ESTree::getPropertyIdentifier(field.setterMethod->_key));
          emitTypedFinalMethodClosureStore(field, field.setterMethod, decl);
        }
      } else {
        auto *funcExpr =
            llvh::cast<ESTree::FunctionExpressionNode>(field.method->_value);
        Value *function = genFunctionExpression(funcExpr, field.name);
        Variable *var = Builder.createVariable(
            curFunction()->curScope()->getVariableScope(),
            field.name,
            flowTypeToIRType(field.type),
            /* hidden */ true);
        Builder.createStoreFrameInst(curFunction()->curScope(), function, var);
        sema::Decl *decl = getIDDecl(
            llvh::cast<ESTree::IdentifierNode>(
                llvh::cast<ESTree::PrivateNameNode>(field.method->_key)->_id));
        setDeclData(decl, var);
      }
    }
  }

  // Emit static methods and fields as variables.
  auto *staticType = classType->getStaticObjectTypeInfo();
  const bool hasDynamicStaticMethods =
      staticType && staticType->hasComputedMethods();
  const bool delayStaticInitializers =
      !computedMethodKeys.empty() || hasDynamicStaticMethods;
  for (ESTree::Node &elem : classBody->_body) {
    if (auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&elem)) {
      if (!method->_static || method->_computed)
        continue;
      assert(staticType && "must have a staticType if there are static fields");
      ESTree::IdentifierNode *id;
      Identifier name;
      bool isPrivate;
      if (auto *pn = llvh::dyn_cast<ESTree::PrivateNameNode>(method->_key)) {
        id = llvh::cast<ESTree::IdentifierNode>(pn->_id);
        name = Mod->getContext().getPrivateNameIdentifier(id->_name);
        isPrivate = true;
      } else {
        id = llvh::cast<ESTree::IdentifierNode>(method->_key);
        name = Identifier::getFromPointer(id->_name);
        isPrivate = false;
      }
      auto *funcExpr =
          llvh::cast<ESTree::FunctionExpressionNode>(method->_value);
      // Generic static methods are emitted through their specializations.
      if (funcExpr->_typeParameters)
        continue;
      Value *function = genFunctionExpression(funcExpr, name);
      auto optField = isPrivate ? staticType->findPrivateField(name)
                                : staticType->findPublicField(name);
      // For accessor methods, use the accessor function type, not the
      // field type (which is the getter return type or void).
      const auto *fieldPtr = optField->getField();
      flow::Type *flowVarType;
      if (method->_kind == kw_.identGet && fieldPtr->getterType)
        flowVarType = fieldPtr->getterType;
      else if (method->_kind == kw_.identSet && fieldPtr->setterType)
        flowVarType = fieldPtr->setterType;
      else if (fieldPtr->isOverloaded()) {
        // For overloaded static methods, look up this method's type in the
        // overloads map (fieldPtr->type is null for overloaded fields).
        flowVarType = fieldPtr->overloads.lookup(method);
        assert(flowVarType && "static method must exist in overloads");
        if (llvh::isa<flow::GenericType>(flowVarType->info))
          continue;
      } else
        flowVarType = fieldPtr->type;
      Variable *var = Builder.createVariable(
          curFunction()->curScope()->getVariableScope(),
          name,
          flowTypeToIRType(flowVarType),
          /* hidden */ true);
      Builder.createStoreFrameInst(curFunction()->curScope(), function, var);
      sema::Decl *decl = semCtx_.getExpressionDecl(id);
      setDeclData(decl, var);
      if (auto *CFI = llvh::dyn_cast<CreateFunctionInst>(function)) {
        declFunctions_.try_emplace(decl, CFI->getFunctionCode());
      }
    } else if (auto *prop = llvh::dyn_cast<ESTree::ClassPropertyNode>(&elem)) {
      if (!prop->_static)
        continue;
      assert(staticType && "must have a staticType if there are static fields");
      auto *id = llvh::cast<ESTree::IdentifierNode>(prop->_key);
      Identifier name = Identifier::getFromPointer(id->_name);
      auto optField = staticType->findPublicField(name);
      assert(optField && "static field must exist in staticObjectType");
      Type irType = flowTypeToIRType(optField->getField()->type);
      Variable *var = Builder.createVariable(
          curFunction()->curScope()->getVariableScope(),
          name,
          irType,
          /* hidden */ true);
      sema::Decl *decl = semCtx_.getExpressionDecl(id);
      setDeclData(decl, var);
      if (!delayStaticInitializers) {
        Value *initValue = prop->_value
            ? genExpression(prop->_value)
            : getDefaultInitValue(optField->getField()->type);
        Builder.createStoreFrameInst(
            curFunction()->curScope(), initValue, var);
      }
    } else if (
        auto *prop = llvh::dyn_cast<ESTree::ClassPrivatePropertyNode>(&elem)) {
      if (!prop->_static)
        continue;
      assert(staticType && "must have a staticType if there are static fields");
      auto *id = llvh::cast<ESTree::IdentifierNode>(prop->_key);
      Identifier name = Mod->getContext().getPrivateNameIdentifier(id->_name);
      auto optField = staticType->findPrivateField(name);
      assert(optField && "private static field must exist in staticObjectType");
      Type irType = flowTypeToIRType(optField->getField()->type);
      Variable *var = Builder.createVariable(
          curFunction()->curScope()->getVariableScope(),
          name,
          irType,
          /* hidden */ true);
      sema::Decl *decl = semCtx_.getExpressionDecl(id);
      setDeclData(decl, var);
      if (!delayStaticInitializers) {
        Value *initValue = prop->_value
            ? genExpression(prop->_value)
            : getDefaultInitValue(optField->getField()->type);
        Builder.createStoreFrameInst(
            curFunction()->curScope(), initValue, var);
      }
    }
  }

  // Create the implicit field initializer function; store the closure
  // for it in a variable, and save that variable in a table indexed by
  // the ClassDeclarationNode.
  emitCreateTypedFieldInitFunction();

  // Emit the explicit constructor, if present.
  Value *consFunction;
  if (ESTree::MethodDefinitionNode *consMethod = semCtx_.getConstructor(node)) {
    // Check that 'super()' call is the first statement in SH for derived
    // classes.
    // TODO: This is intentionally overly restrictive. In the future, we can
    // check that super() call happens prior to any function calls or 'this'
    // accesses.
    if (ESTree::getSuperClass(node)) {
      // Attempt to extract the super() call from the first statement of the
      // block.
      ESTree::NodeList &blockStmtBody =
          llvh::cast<ESTree::BlockStatementNode>(
              llvh::cast<ESTree::FunctionExpressionNode>(consMethod->_value)
                  ->_body)
              ->_body;
      ESTree::Node *firstStatement = nullptr;
      for (ESTree::Node &it : blockStmtBody) {
        firstStatement = &it;
        auto *exprSt = llvh::dyn_cast<ESTree::ExpressionStatementNode>(&it);
        // Skip directives.
        if (!exprSt || !exprSt->_directive) {
          break;
        }
      }
      ESTree::ExpressionStatementNode *exprStatement =
          llvh::dyn_cast_or_null<ESTree::ExpressionStatementNode>(
              firstStatement);
      ESTree::CallExpressionNode *superCall = exprStatement
          ? llvh::dyn_cast<ESTree::CallExpressionNode>(
                exprStatement->_expression)
          : nullptr;
      if (!superCall || !llvh::isa<ESTree::SuperNode>(superCall->_callee)) {
        Mod->getContext().getSourceErrorManager().error(
            node->getSourceRange(),
            "first statement in derived class constructor must be super();");
      }
    }

    consFunction = genFunctionExpression(
        llvh::cast<ESTree::FunctionExpressionNode>(consMethod->_value),
        consName,
        ESTree::getSuperClass(node),
        ESTree::getSuperClass(node)
            ? Function::DefinitionKind::ES6DerivedConstructor
            : Function::DefinitionKind::ES6BaseConstructor);
  } else {
    // The constructor is implicit.
    consFunction = genTypedImplicitConstructor(consName, superClass);
  }
  Variable *constructorVar;
  if (constructorDecl) {
    constructorVar = llvh::cast<Variable>(getDeclData(constructorDecl));
    emitStore(consFunction, constructorVar, true);
  } else {
    constructorVar = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        Builder.createIdentifier("?class.constructor"),
        Type::createObject(),
        /* hidden */ true);
    Builder.createStoreFrameInst(
        curFunction()->curScope(), consFunction, constructorVar);
  }

  // Create and populate the "prototype" property (vtable).
  // Must be done even if there are no methods to enable 'instanceof'.
  Value *vtable;
  if (superClass) {
    auto it = classConstructors_.find(classType->getSuperClassInfo());
    assert(it != classConstructors_.end() && "missing super class constructor");
    auto *RSI =
        emitResolveScopeInstIfNeeded(it->second.homeObjectVar->getParent());
    vtable = Builder.createLoadFrameInst(RSI, it->second.homeObjectVar);
    // TODO: This will be known to be the actual type when we properly use an
    // instruction for class creation, but for now we need an object here
    // because we want to use PrLoad on it.
    vtable->setType(Type::createObject());
  } else if (flowContext_.isArrayClassType(consType->getClassType())) {
    // Typed arrays retain their specialized methods while inheriting standard
    // array behavior, including iteration when they flow into dynamic code.
    vtable = Builder.createLoadPropertyInst(
        Builder.createTryLoadGlobalPropertyInst("Array"), "prototype");
    vtable->setType(Type::createObject());
  } else {
    vtable = Builder.getLiteralNull();
  }

  const bool hasDynamicInstanceMethods =
      classType->getHomeObjectTypeInfo()->hasComputedMethods();
  llvh::DenseMap<ESTree::MethodDefinitionNode *, Value *>
      emittedMethodFunctions;
  auto *homeObject = emitTypedClassAllocation(
      classType->getHomeObjectTypeInfo(),
      vtable,
      /* skipPrivateFields */ true,
      /* propertiesEnumerable */ false,
      /* allowDynamicProperties */ hasDynamicInstanceMethods,
      hasDynamicInstanceMethods ? &emittedMethodFunctions : nullptr);

  // Store the home object before compiling computed methods so it can serve
  // as their [[HomeObject]]. Check if a previous compilation (for example, a
  // finally block) already created the variable.
  Variable *homeObjectVar;
  auto existingIt = classConstructors_.find(classType);
  if (existingIt != classConstructors_.end()) {
    homeObjectVar = existingIt->second.homeObjectVar;
  } else {
    homeObjectVar = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        Builder.createIdentifier(
            llvh::Twine("?") +
            (consName.isValid() ? consName.str() : llvh::StringRef("class")) +
            ".prototype"),
        flowTypeToIRType(classType->getHomeObjectType()),
        /* hidden */ true);
  }
  Builder.createStoreFrameInst(
      curFunction()->curScope(), homeObject, homeObjectVar);

  auto loadStoredMethod = [this](ESTree::MethodDefinitionNode *method) {
    auto *id = ESTree::getPropertyIdentifier(method->_key);
    auto *methodDecl = semCtx_.getExpressionDecl(id);
    assert(
        methodDecl && methodDecl->customData &&
        "stored method must have an associated variable");
    return emitLoad(getDeclData(methodDecl), false);
  };

  // A computed method in a subclass can replace an inherited final method or
  // accessor. Those methods normally exist only in scope variables, so expose
  // them on the dynamic home object before applying this class's definitions.
  if (hasComputedInstanceMethods) {
    auto *homeType = classType->getHomeObjectTypeInfo();
    for (const auto &[name, entry] : homeType->getFieldNameMap()) {
      const auto *field = entry.getField();
      if (entry.classType == homeType || !field->finalMethod)
        continue;

      auto *key = Builder.getLiteralString(name);
      if (field->isAccessor()) {
        Value *getter = Builder.getLiteralUndefined();
        if (field->method)
          getter = loadStoredMethod(field->method);
        Value *setter = Builder.getLiteralUndefined();
        if (field->setterMethod)
          setter = loadStoredMethod(field->setterMethod);
        Builder.createDefineOwnGetterSetterInst(
            getter,
            setter,
            homeObject,
            key,
            IRBuilder::PropEnumerable::No);
        continue;
      }

      if (field->isOverloaded()) {
        for (const auto &[overloadMethod, overloadType] : field->overloads) {
          if (llvh::isa<flow::GenericType>(overloadType->info))
            continue;
          Builder.createDefineOwnPropertyInst(
              loadStoredMethod(overloadMethod),
              homeObject,
              key,
              IRBuilder::PropEnumerable::No);
        }
        continue;
      }

      Builder.createDefineOwnPropertyInst(
          loadStoredMethod(field->method),
          homeObject,
          key,
          IRBuilder::PropEnumerable::No);
    }
  }

  // Install methods in source order. Replaying named methods is necessary
  // because a computed key can replace a named property in either direction.
  for (ESTree::Node &elem : classBody->_body) {
    auto *method = llvh::dyn_cast<ESTree::MethodDefinitionNode>(&elem);
    if (!method || method->_kind == kw_.identConstructor ||
        llvh::isa<ESTree::PrivateNameNode>(method->_key)) {
      continue;
    }

    Value *key;
    Value *function;
    if (method->_computed) {
      key = computedMethodKeys.lookup(method);
      assert(key && "computed method key must have been evaluated");
      function = genFunctionExpression(
          llvh::cast<ESTree::FunctionExpressionNode>(method->_value),
          Identifier{},
          ESTree::getSuperClass(node),
          Function::DefinitionKind::ES6Method,
          method->_static ? constructorVar : homeObjectVar,
          method);
      genBuiltinCall(
          BuiltinMethod::HermesBuiltin_setFunctionName,
          {function, key, Builder.getLiteralNumber(0)});
    } else {
      if ((method->_static && !hasDynamicStaticMethods) ||
          (!method->_static && !hasDynamicInstanceMethods)) {
        continue;
      }

      auto *funcExpr =
          llvh::cast<ESTree::FunctionExpressionNode>(method->_value);
      if (funcExpr->_typeParameters)
        continue;

      auto *id = llvh::cast<ESTree::IdentifierNode>(method->_key);
      key = Builder.getLiteralString(Identifier::getFromPointer(id->_name));
      if (method->_static) {
        function = loadStoredMethod(method);
      } else {
        auto optField = classType->getHomeObjectTypeInfo()->findPublicField(
            Identifier::getFromPointer(id->_name));
        assert(optField && "instance method must exist in home object type");
        function = optField->getField()->finalMethod
            ? loadStoredMethod(method)
            : emittedMethodFunctions.lookup(method);
        assert(function && "instance method closure must have been emitted");
      }
    }

    Value *target = method->_static ? consFunction : homeObject;
    if (method->_kind == kw_.identGet) {
      Builder.createDefineOwnGetterSetterInst(
          function,
          Builder.getLiteralUndefined(),
          target,
          key,
          IRBuilder::PropEnumerable::No);
    } else if (method->_kind == kw_.identSet) {
      Builder.createDefineOwnGetterSetterInst(
          Builder.getLiteralUndefined(),
          function,
          target,
          key,
          IRBuilder::PropEnumerable::No);
    } else {
      Builder.createDefineOwnPropertyInst(
          function, target, key, IRBuilder::PropEnumerable::No);
    }
  }

  // Static field values are evaluated after every method has been installed.
  // Their Variables and Decls were created in the earlier static-member pass,
  // so initializers can refer to any static member of this class.
  if (delayStaticInitializers) {
    for (ESTree::Node &elem : classBody->_body) {
      if (auto *prop = llvh::dyn_cast<ESTree::ClassPropertyNode>(&elem)) {
        if (!prop->_static)
          continue;
        auto *id = llvh::cast<ESTree::IdentifierNode>(prop->_key);
        Identifier name = Identifier::getFromPointer(id->_name);
        auto optField = staticType->findPublicField(name);
        assert(optField && "static field must exist in staticObjectType");
        auto *var = llvh::cast<Variable>(
            getDeclData(semCtx_.getExpressionDecl(id)));
        Value *initValue = prop->_value
            ? genExpression(prop->_value)
            : getDefaultInitValue(optField->getField()->type);
        Builder.createStoreFrameInst(
            curFunction()->curScope(), initValue, var);
        if (hasDynamicStaticMethods) {
          Builder.createDefineOwnPropertyInst(
              initValue,
              consFunction,
              Builder.getLiteralString(name),
              IRBuilder::PropEnumerable::Yes);
        }
        continue;
      }

      auto *privateProp =
          llvh::dyn_cast<ESTree::ClassPrivatePropertyNode>(&elem);
      if (!privateProp || !privateProp->_static)
        continue;
      auto *id = llvh::cast<ESTree::IdentifierNode>(privateProp->_key);
      Identifier name = Mod->getContext().getPrivateNameIdentifier(id->_name);
      auto optField = staticType->findPrivateField(name);
      assert(optField && "private static field must exist in staticObjectType");
      auto *var = llvh::cast<Variable>(
          getDeclData(semCtx_.getExpressionDecl(id)));
      Value *initValue = privateProp->_value
          ? genExpression(privateProp->_value)
          : getDefaultInitValue(optField->getField()->type);
      Builder.createStoreFrameInst(
          curFunction()->curScope(), initValue, var);
    }
  }

  // Handle generic method specializations.
  // Compile each specialization, create a Variable, and store the closure.
  for (const auto &[specializedMethod, decl] :
       classType->getSpecializedMethodDecls()) {
    auto *specializedFE =
        llvh::cast<ESTree::FunctionExpressionNode>(specializedMethod->_value);
    auto *methodId = ESTree::getPropertyIdentifier(specializedMethod->_key);
    Identifier name;
    if (llvh::isa<ESTree::PrivateNameNode>(specializedMethod->_key)) {
      name = Mod->getContext().getPrivateNameIdentifier(methodId->_name);
    } else {
      name = Identifier::getFromPointer(methodId->_name);
    }
    Value *function = genFunctionExpression(specializedFE, name);
    // Create a variable to hold the specialized closure.
    Variable *var = Builder.createVariable(
        curFunction()->curScope()->getVariableScope(),
        decl->name,
        Type::createObject(),
        /* hidden */ true);
    Builder.createStoreFrameInst(curFunction()->curScope(), function, var);
    setDeclData(decl, var);
    if (auto *CFI = llvh::dyn_cast<CreateFunctionInst>(function)) {
      declFunctions_.try_emplace(decl, CFI->getFunctionCode());
    }
  }

  // Check to make sure this is a valid class definition,
  // because there may have been errors.
  if (auto *createCallable =
          llvh::dyn_cast<BaseCreateCallableInst>(consFunction)) {
    auto [it, inserted] = classConstructors_.try_emplace(
        classType, createCallable->getFunctionCode(), homeObjectVar);
    (void)it;
    (void)inserted;
    // On recompilation, the entry already exists. Verify consistency.
    assert(
        it->second.constructorFunc == createCallable->getFunctionCode() &&
        "redefinition of constructor function");
    assert(
        it->second.homeObjectVar == homeObjectVar &&
        "redefinition with different homeObjectVar");
  }

  // The 'prototype' property is initially set as non-configurable,
  // and we're overwriting it with our own.
  // So we can't use StoreOwnProperty here because that attempts to define a
  // configurable property.
  // TODO: Do this properly by using a new instruction for class creation.
  Builder.createStorePropertyStrictInst(
      homeObject,
      consFunction,
      Builder.getLiteralString(kw_.identPrototype->str()));
  return consFunction;
}

CreateFunctionInst *ESTreeIRGen::genTypedImplicitConstructor(
    const Identifier &consName,
    Value *superClass) {
  Function *func;

  // Use the compiledEntities_ cache even though we're not enqueuing a
  // function compilation (because the function is trivial).
  // This way we avoid making multiple implicit constructors for the same
  // classType, allowing us to populate the target operand of CallInsts.
  if (Value *found = findCompiledEntity(
          curFunction()->typedClassContext.node,
          ExtraKey::ImplicitClassConstructor)) {
    func = llvh::cast<Function>(found);
  } else {
    IRBuilder::SaveRestore saveState{Builder};

    // Retrieve the FunctionInfo for the implicit constructor, which must exist.
    sema::FunctionInfo *funcInfo =
        ESTree::getDecoration<ESTree::ClassLikeDecoration>(
            curFunction()->typedClassContext.node)
            ->implicitCtorFunctionInfo;
    assert(
        funcInfo &&
        "Semantic resolver failed to decorate class with implicit ctor");

    // AST Node for the superClass, null if no superclass.
    ESTree::Node *superClassNode = superClass
        ? ESTree::getSuperClass(curFunction()->typedClassContext.node)
        : nullptr;

    // Determine if the implicit constructor needs to emit a super() call.
    // This is needed only if some ancestor has an explicit constructor or
    // field initializers that must be triggered.
    bool needsSuperCall = false;
    if (superClassNode) {
      for (auto *cur =
               curFunction()->typedClassContext.type->getSuperClassInfo();
           cur;
           cur = cur->getSuperClassInfo()) {
        if (cur->getConstructorType() || classFieldInitInfo_.count(cur)) {
          needsSuperCall = true;
          break;
        }
      }
    }

    func = Builder.createFunction(
        consName,
        needsSuperCall ? Function::DefinitionKind::ES6DerivedConstructor
                       : Function::DefinitionKind::ES5Function,
        true,
        funcInfo->customDirectives);

    auto compileFunc = [this,
                        func,
                        funcInfo,
                        superClassNode,
                        needsSuperCall,
                        typedClassContext = curFunction()->typedClassContext,
                        parentScope =
                            curFunction()->curScope()->getVariableScope()] {
      FunctionContext newFunctionContext{this, func, funcInfo};
      newFunctionContext.typedClassContext = typedClassContext;
      newFunctionContext.superClassNode_ = superClassNode;

      auto *prologueBB = Builder.createBasicBlock(func);
      Builder.setInsertionBlock(prologueBB);

      emitFunctionPrologue(
          nullptr,
          prologueBB,
          InitES5CaptureState::No,
          DoEmitDeclarations::No,
          parentScope);

      if (needsSuperCall) {
        // Generate implicit super call forwarding the parameters.
        // Walk up the class chain to find the nearest explicit constructor.
        flow::TypedFunctionType *superCtorTypeInfo = nullptr;
        for (auto *cur = typedClassContext.type->getSuperClassInfo(); cur;
             cur = cur->getSuperClassInfo()) {
          if (cur->getConstructorType()) {
            superCtorTypeInfo = cur->getConstructorTypeInfo();
            break;
          }
        }

        // Forward the super constructor's parameters, if any.
        CallInst::ArgumentList args;
        if (superCtorTypeInfo) {
          auto params = superCtorTypeInfo->getParams();
          for (const flow::TypedFunctionType::Param &param : params) {
            auto *jsParam = Builder.createJSDynamicParam(func, param.name);
            jsParam->setType(flowTypeToIRType(param.type));
            auto *loadParam = Builder.createLoadParamInst(jsParam);
            curFunction()->jsParams.push_back(loadParam);
            args.push_back(loadParam);
          }
          // +1 for 'this'.
          func->setExpectedParamCountIncludingThis(params.size() + 1);
        }

        // Load the super class callee and call it.
        Value *callee = genExpression(superClassNode);
        Value *thisVal = curFunction()->jsParams[0];
        Value *newTarget =
            Builder.createGetNewTargetInst(func->getNewTargetParam());

        Builder.createCallInst(
            callee,
            /* target */ Builder.getEmptySentinel(),
            /* calleeIsAlwaysClosure */ true,
            Builder.getEmptySentinel(),
            newTarget,
            thisVal,
            args);
      }

      emitTypedFieldInitCall(curFunction()->typedClassContext.type);

      emitFunctionEpilogue(Builder.getLiteralUndefined());
    };
    enqueueCompilation(
        curFunction()->typedClassContext.node,
        ExtraKey::ImplicitClassConstructor,
        func,
        compileFunc);
  }

  return Builder.createCreateFunctionInst(curFunction()->curScope(), func);
}

Value *ESTreeIRGen::emitTypedClassAllocation(
    flow::ClassType *classType,
    Value *parent,
    bool skipPrivateFields,
    bool propertiesEnumerable,
    bool allowDynamicProperties,
    llvh::DenseMap<ESTree::MethodDefinitionNode *, Value *>
        *emittedMethodFunctions) {
  assert(parent && "parent must be specified");
  assert(
      (!allowDynamicProperties || !propertiesEnumerable) &&
      "dynamic class properties are only supported on home objects");
  assert(
      (allowDynamicProperties || !emittedMethodFunctions) &&
      "method closures are only needed for dynamic home objects");
  // TODO: should create a sealed object, etc.
  AllocTypedObjectInst::ObjectPropertyMap propMap{};

  // Number of layout slots, which includes both public and private fields.
  // Final methods don't get layout slots (they are stored in Variables),
  // so we use getNumLayoutSlots() instead of getFieldNameMap().size().
  size_t numFields = classType->getNumLayoutSlots();
  propMap.resize(numFields);
  auto addField =
      [this,
       &propMap,
       classType,
       parent,
       allowDynamicProperties,
       emittedMethodFunctions](
          const flow::ClassType::FieldLookupEntry &entry) {
    const flow::ClassType::Field &field = *entry.getField();

    // Verify that each layout slot is filled exactly once.
    assert(
        (!field.layoutSlotIR.hasValue() ||
         propMap[*field.layoutSlotIR].first == nullptr) &&
        "every layout slot must be filled exactly once");

    Literal *name = field.isPrivate
        ? Builder.getLiteralPrivateName(field.name)
        : static_cast<Literal *>(Builder.getLiteralString(field.name));

    if (field.isMethod()) {
      // Final methods don't get a layout slot in the home object.
      // They are accessed through Variables instead.
      if (field.finalMethod) {
        if (field.isOverloaded()) {
          // Overloaded final method: emit each overload as a separate
          // closure.
          if (entry.classType == classType) {
            // Field was defined in this class (not inherited).
            // Emit the code here.
            auto getFinalDecl = [this](ESTree::MethodDefinitionNode *m) {
              return semCtx_.getExpressionDecl(
                  ESTree::getPropertyIdentifier(m->_key));
            };
            for (auto &[overloadMethod, overloadType] : field.overloads) {
              if (llvh::isa<flow::GenericType>(overloadType->info)) {
                // Generic overload — codegen happens via specializations
                // emitted from the call site (see FlowChecker overload
                // resolution).
                continue;
              }
              emitTypedFinalMethodClosureStore(
                  field, overloadMethod, getFinalDecl(overloadMethod));
            }
          }
          return;
        }
        if (field.type && llvh::isa<flow::GenericType>(field.type->info)) {
          // Generic final method - no codegen needed here.
          return;
        }
        if (entry.classType == classType) {
          // Non-generic final method defined in this class.
          auto getFinalDecl = [&](ESTree::MethodDefinitionNode *m) {
            return semCtx_.getExpressionDecl(
                ESTree::getPropertyIdentifier(m->_key));
          };
          if (field.method) {
            emitTypedFinalMethodClosureStore(
                field, field.method, getFinalDecl(field.method));
          }
          if (field.setterMethod) {
            emitTypedFinalMethodClosureStore(
                field, field.setterMethod, getFinalDecl(field.setterMethod));
          }
        }
      } else if (entry.classType == classType) {
        // Not declared as a final method.
        // Create the code for the method.
        Value *function = genFunctionExpression(
            llvh::cast<ESTree::FunctionExpressionNode>(field.method->_value),
            field.name);
        propMap[*field.layoutSlotIR] = {name, function};
        if (emittedMethodFunctions) {
          emittedMethodFunctions->try_emplace(field.method, function);
        }
        if (auto *CFI = llvh::dyn_cast<CreateFunctionInst>(function)) {
          // If this field represents a non-overridden method, record the
          // IR function so we can populate the target of calls.
          if (!field.overridden && !allowDynamicProperties) {
            auto [it, success] = nonOverriddenMethods_.try_emplace(
                &field, CFI->getFunctionCode());
            (void)it;
            (void)success;
            // On recompilation (e.g., finally blocks), the entry may already
            // exist. Verify it has the same Function.
            assert(
                (success || it->second == CFI->getFunctionCode()) &&
                "Method already emitted with different function");
          }
        }
      } else {
        assert(parent && "inherited field without parent ClassType");
        // Method is inherited. Read it from the parent.
        propMap[*field.layoutSlotIR] = {
            name,
            Builder.createPrLoadInst(
                parent,
                *field.layoutSlotIR,
                name,
                flowTypeToIRType(field.type))};
      }
    } else {
      // Class element is a field.
      // Need to emit an IDZ check for types that can't have a primitive
      // default.
      Value *initValue =
          getTypeContext().canBePrimitive(flowTypeToIRType(field.type))
          ? getDefaultInitValue(field.type)
          : Builder.getLiteralUninit();
      propMap[*field.layoutSlotIR] = {name, initValue};
    }
  };

  // Generate code for each field, place it in the propMap.
  for (const auto &it : classType->getFieldNameMap()) {
    flow::ClassType::FieldLookupEntry entry = it.second;
    addField(entry);
  }
  // Private fields are not stored in the privateFieldNameMap,
  // so we need to iterate over superclasses to find them.
  if (!skipPrivateFields) {
    for (auto *cur = classType; cur; cur = cur->getSuperClassInfo()) {
      for (const auto &[name, idx] : cur->getPrivateFieldNameMap()) {
        flow::ClassType::FieldLookupEntry entry{cur, idx};
        addField(entry);
      }
    }
  }

  if (propertiesEnumerable)
    return Builder.createAllocTypedObjectInst(propMap, parent);
  if (allowDynamicProperties) {
    // Typed objects have fixed hidden classes and are non-extensible at
    // runtime. Build an ordinary object in the same slot order when computed
    // methods need to be installed dynamically. PrLoad continues to use the
    // statically assigned slots.
    Value *object = Builder.createAllocObjectLiteralInst({}, parent);
    for (const auto &[name, value] : propMap) {
      assert(name && value && "class layout slot must be initialized");
      Builder.createDefineOwnPropertyInst(
          value, object, name, IRBuilder::PropEnumerable::No);
    }
    return object;
  }
  return Builder.createAllocTypedNonEnumObjectInst(propMap, parent);
}

Value *ESTreeIRGen::getDefaultInitValue(flow::Type *type) {
  switch (type->info->getKind()) {
    case flow::TypeKind::Void:
      return Builder.getLiteralUndefined();
    case flow::TypeKind::Null:
      return Builder.getLiteralNull();
    case flow::TypeKind::Boolean:
      return Builder.getLiteralBool(false);
    case flow::TypeKind::String:
      return Builder.getLiteralString("");
    case flow::TypeKind::CPtr:
    case flow::TypeKind::Number:
      return Builder.getLiteralPositiveZero();
    case flow::TypeKind::BigInt:
      return Builder.getLiteralBigInt(
          Mod->getContext().getIdentifier("0").getUnderlyingPointer());
    case flow::TypeKind::Any:
    case flow::TypeKind::Empty:
    case flow::TypeKind::Mixed:
      return Builder.getLiteralUndefined();
    case flow::TypeKind::Union:
      return getDefaultInitValue(
          llvh::cast<flow::UnionType>(type->info)->getTypes()[0]);
    case flow::TypeKind::TypedFunction:
    case flow::TypeKind::NativeFunction:
    case flow::TypeKind::UntypedFunction:
    case flow::TypeKind::Class:
    case flow::TypeKind::ClassConstructor:
    case flow::TypeKind::Array:
    case flow::TypeKind::Tuple:
    case flow::TypeKind::ExactObject:
      return Builder.getLiteralPositiveZero();
    case flow::TypeKind::Generic:
    case flow::TypeKind::InferencePlaceholder:
    case flow::TypeKind::InferencePlaceholderArray:
      hermes_fatal("invalid typekind");
  }
  llvm_unreachable("all cases handled");
}

Type ESTreeIRGen::flowTypeToIRType(flow::TypeInfo *flowType) {
  switch (flowType->getKind()) {
    case flow::TypeKind::Void:
      return Type::createUndefined();
    case flow::TypeKind::Null:
      return Type::createNull();
    case flow::TypeKind::Boolean:
      return Type::createBoolean();
    case flow::TypeKind::String:
      return Type::createString();
    case flow::TypeKind::CPtr:
    case flow::TypeKind::Number:
      return Type::createNumber();
    case flow::TypeKind::BigInt:
      return Type::createBigInt();
    case flow::TypeKind::Any:
    case flow::TypeKind::Empty:
    case flow::TypeKind::Mixed:
      return Type::createAnyType();
    case flow::TypeKind::Union: {
      TypeContext &tc = getTypeContext();
      Type res = Type::createNoType();
      for (flow::Type *elemType :
           llvh::cast<flow::UnionType>(flowType)->getTypes()) {
        res = tc.unionTy(res, flowTypeToIRType(elemType));
      }
      return res;
    }
    case flow::TypeKind::NativeFunction:
      return Type::createNumber();
    case flow::TypeKind::TypedFunction:
    case flow::TypeKind::UntypedFunction:
      return Type::createObject();
    case flow::TypeKind::Class:
      return Type::createObject();
    case flow::TypeKind::ClassConstructor:
      return Type::createObject();
    case flow::TypeKind::Array:
    case flow::TypeKind::Tuple:
    case flow::TypeKind::ExactObject:
      return Type::createObject();
    case flow::TypeKind::Generic:
    case flow::TypeKind::InferencePlaceholder:
    case flow::TypeKind::InferencePlaceholderArray:
      hermes_fatal("invalid typekind");
  }
  llvm_unreachable("all cases handled");
}

} // namespace irgen
} // namespace hermes
