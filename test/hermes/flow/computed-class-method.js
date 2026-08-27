/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Werror -typed -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -Werror -typed -exec -O0 %s | %FileCheck --match-full-lines %s

'use strict';

let keyOrder: string = '';
let keyConversions: number = 0;

let objectKey: any = {};
objectKey.toString = function(): string {
  keyOrder = keyOrder + 'convert,';
  keyConversions++;
  return 'instanceMethod';
};
let symbolKey: any = Symbol('symbolMethod');

function recordKey(label: string, value: any): any {
  keyOrder = keyOrder + label;
  return value;
}

function recordInitializer(): number {
  keyOrder = keyOrder + 'initialize';
  return ((ComputedMethods: any).staticMethod(0): number);
}

class Parent {
  prefix: string = 'value';

  format(value: number): string {
    return this.prefix + ':' + String(value);
  }
}

class ComputedMethods extends Parent {
  value: number = 40;

  [recordKey('instance,', objectKey)](increment: number): string {
    return super.format(this.value + increment);
  }

  static initialized: number = recordInitializer();

  static [recordKey('static,', 'staticMethod')](value: number): number {
    return value + 1;
  }

  [recordKey('symbol,', symbolKey)](): string {
    return 'symbol result';
  }
}

print(keyOrder);
// CHECK: instance,convert,static,symbol,initialize
print(keyConversions);
// CHECK-NEXT: 1
print(ComputedMethods.initialized);
// CHECK-NEXT: 1

let dynamicClass: any = ComputedMethods;
let instance: any = new ComputedMethods();

print(instance.instanceMethod(2));
// CHECK-NEXT: value:42
print(dynamicClass.staticMethod(6));
// CHECK-NEXT: 7
print(instance[symbolKey]());
// CHECK-NEXT: symbol result

print(instance.instanceMethod.name);
// CHECK-NEXT: instanceMethod
print(dynamicClass.staticMethod.name);
// CHECK-NEXT: staticMethod
print(instance[symbolKey].name);
// CHECK-NEXT: [symbolMethod]

print(
  Object.prototype.hasOwnProperty.call(
    dynamicClass.prototype,
    'instanceMethod',
  ),
  Object.prototype.hasOwnProperty.call(dynamicClass, 'instanceMethod'),
);
// CHECK-NEXT: true false
print(
  Object.prototype.hasOwnProperty.call(dynamicClass, 'staticMethod'),
  Object.prototype.hasOwnProperty.call(
    dynamicClass.prototype,
    'staticMethod',
  ),
);
// CHECK-NEXT: true false

let instanceDescriptor: any = Object.getOwnPropertyDescriptor(
  dynamicClass.prototype,
  'instanceMethod',
);
let staticDescriptor: any = Object.getOwnPropertyDescriptor(
  dynamicClass,
  'staticMethod',
);
print(
  instanceDescriptor.enumerable,
  instanceDescriptor.writable,
  instanceDescriptor.configurable,
);
// CHECK-NEXT: false true true
print(
  staticDescriptor.enumerable,
  staticDescriptor.writable,
  staticDescriptor.configurable,
);
// CHECK-NEXT: false true true

let collisionKey: any = 'method';
class CollidingMethods {
  method(): string {
    return 'named';
  }

  [collisionKey](): string {
    return 'computed';
  }
}
print(new CollidingMethods().method());
// CHECK-NEXT: computed

let expressionKeyEvaluations: number = 0;
function expressionKey(): string {
  expressionKeyEvaluations++;
  return 'expressionMethod';
}
let ExpressionClass: any = class {
  [expressionKey()](): string {
    return 'expression result';
  }
};
let expressionInstance: any = new ExpressionClass();
print(expressionKeyEvaluations);
// CHECK-NEXT: 1
print(expressionInstance.expressionMethod());
// CHECK-NEXT: expression result
print(expressionInstance.expressionMethod.name);
// CHECK-NEXT: expressionMethod
