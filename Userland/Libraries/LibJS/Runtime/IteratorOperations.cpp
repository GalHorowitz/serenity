/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/IteratorOperations.h>

namespace JS {

// 7.4.1 GetIterator ( obj [ , hint [ , method ] ] ), https://tc39.es/ecma262/#sec-getiterator
Object* get_iterator(GlobalObject& global_object, Value value, IteratorHint hint, Value method)
{
    auto& vm = global_object.vm();
    if (method.is_empty()) {
        if (hint == IteratorHint::Async)
            TODO();
        auto object = value.to_object(global_object);
        if (!object)
            return {};
        method = object->get(*vm.well_known_symbol_iterator());
        if (vm.exception())
            return {};
    }
    if (!method.is_function()) {
        vm.throw_exception<TypeError>(global_object, ErrorType::NotIterable, value.to_string_without_side_effects());
        return nullptr;
    }
    auto iterator = TRY_OR_DISCARD(vm.call(method.as_function(), value));
    if (!iterator.is_object()) {
        vm.throw_exception<TypeError>(global_object, ErrorType::NotIterable, value.to_string_without_side_effects());
        return nullptr;
    }
    return &iterator.as_object();
}

// 7.4.2 IteratorNext ( iteratorRecord [ , value ] ), https://tc39.es/ecma262/#sec-iteratornext
Object* iterator_next(Object& iterator, Value value)
{
    auto& vm = iterator.vm();
    auto& global_object = iterator.global_object();

    auto next_method = iterator.get(vm.names.next);
    if (vm.exception())
        return {};

    if (!next_method.is_function()) {
        vm.throw_exception<TypeError>(global_object, ErrorType::IterableNextNotAFunction);
        return nullptr;
    }

    Value result;
    if (value.is_empty())
        result = TRY_OR_DISCARD(vm.call(next_method.as_function(), &iterator));
    else
        result = TRY_OR_DISCARD(vm.call(next_method.as_function(), &iterator, value));

    if (!result.is_object()) {
        vm.throw_exception<TypeError>(global_object, ErrorType::IterableNextBadReturn);
        return nullptr;
    }

    return &result.as_object();
}

// 7.4.3 IteratorComplete ( iterResult ), https://tc39.es/ecma262/#sec-iteratorcomplete
bool iterator_complete(GlobalObject& global_object, Object& iterator_result)
{
    auto& vm = global_object.vm();
    auto done = iterator_result.get(vm.names.done);
    if (vm.exception())
        return {};
    return done.to_boolean();
}

// 7.4.4 IteratorValue ( iterResult ), https://tc39.es/ecma262/#sec-iteratorvalue
Value iterator_value(GlobalObject& global_object, Object& iterator_result)
{
    auto& vm = global_object.vm();
    auto value = iterator_result.get(vm.names.value);
    if (vm.exception())
        return {};
    return value;
}

// 7.4.5 IteratorStep ( iteratorRecord ), https://tc39.es/ecma262/#sec-iteratorstep
Object* iterator_step(GlobalObject& global_object, Object& iterator)
{
    auto& vm = global_object.vm();

    auto result = iterator_next(iterator);
    if (vm.exception())
        return {};

    auto done = iterator_complete(global_object, *result);
    if (vm.exception())
        return {};

    if (done)
        return nullptr;

    return result;
}

// 7.4.6 IteratorClose ( iteratorRecord, completion ), https://tc39.es/ecma262/#sec-iteratorclose
void iterator_close(Object& iterator)
{
    auto& vm = iterator.vm();
    auto& global_object = iterator.global_object();

    // Emulates `completion` behavior
    auto* completion_exception = vm.exception();
    vm.clear_exception();
    auto unwind_until = vm.unwind_until();
    auto unwind_until_label = vm.unwind_until_label();
    vm.stop_unwind();
    auto restore_completion = [&]() {
        if (completion_exception)
            vm.set_exception(*completion_exception);
        if (unwind_until != ScopeType::None)
            vm.unwind(unwind_until, unwind_until_label);
    };

    auto return_method_or_error = Value(&iterator).get_method(global_object, vm.names.return_);
    Value result;
    if (!return_method_or_error.is_error()) { // If innerResult.[[Type]] is normal, then
        auto return_method = return_method_or_error.release_value();
        if (!return_method)
            return restore_completion(); // If return is undefined, return Completion(completion).
        auto result_or_error = vm.call(*return_method, &iterator);
        if (result_or_error.is_error())
            return_method_or_error = result_or_error.release_error();
        else
            result = result_or_error.release_value();
    }
    if (completion_exception)
        return restore_completion(); // If completion.[[Type]] is throw, return Completion(completion).
    if (return_method_or_error.is_error())
        return; // If innerResult.[[Type]] is throw, return Completion(innerResult).
    if (!result.is_object()) {
        vm.throw_exception<TypeError>(global_object, ErrorType::IterableReturnBadReturn);
        return; // If Type(innerResult.[[Value]]) is not Object, throw a TypeError exception.
    }
    restore_completion(); // Return Completion(completion).
}

// 7.4.8 CreateIterResultObject ( value, done ), https://tc39.es/ecma262/#sec-createiterresultobject
Object* create_iterator_result_object(GlobalObject& global_object, Value value, bool done)
{
    auto& vm = global_object.vm();
    auto* object = Object::create(global_object, global_object.object_prototype());
    object->create_data_property_or_throw(vm.names.value, value);
    object->create_data_property_or_throw(vm.names.done, Value(done));
    return object;
}

// 7.4.10 IterableToList ( items [ , method ] ), https://tc39.es/ecma262/#sec-iterabletolist
MarkedValueList iterable_to_list(GlobalObject& global_object, Value iterable, Value method)
{
    auto& vm = global_object.vm();
    MarkedValueList values(vm.heap());
    get_iterator_values(
        global_object, iterable, [&](auto value) {
            if (vm.exception())
                return IterationDecision::Break;
            values.append(value);
            return IterationDecision::Continue;
        },
        method, CloseOnAbrupt::No);
    return values;
}

void get_iterator_values(GlobalObject& global_object, Value value, Function<IterationDecision(Value)> callback, Value method, CloseOnAbrupt close_on_abrupt)
{
    auto& vm = global_object.vm();

    auto iterator = get_iterator(global_object, value, IteratorHint::Sync, method);
    if (!iterator)
        return;

    while (true) {
        auto next_object = iterator_next(*iterator);
        if (!next_object)
            return;

        auto done_property = next_object->get(vm.names.done);
        if (vm.exception())
            return;

        if (!done_property.is_empty() && done_property.to_boolean())
            return;

        auto next_value = next_object->get(vm.names.value);
        if (vm.exception())
            return;

        auto result = callback(next_value);
        if (result == IterationDecision::Break) {
            if (close_on_abrupt == CloseOnAbrupt::Yes)
                iterator_close(*iterator);
            return;
        }
        VERIFY(result == IterationDecision::Continue);
    }
}

}
