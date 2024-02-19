#include "Common.hpp"
#include "Checker.hpp"
#include <sstream>
#include <format>
#include <iostream>

Opt<Error> typecheck_namespace(const ParsedNamespace& parsed_namespace, ScopeId scope_id, Project& project) {
    Opt<Error> error = std::nullopt;

    RecordId project_record_length = project.records.size();

    for (auto ns : parsed_namespace.namespaces) {
        ScopeId ns_scope_id = project.create_scope(scope_id);
        project.scopes[ns_scope_id]->namespace_name = ns->name;
        project.scopes[scope_id]->children.push_back(ns_scope_id);
        typecheck_namespace(*ns, ns_scope_id, project);
    }

    for (RecordId id = 0; id < parsed_namespace.objects.size(); id++) {
        auto object = parsed_namespace.objects[id];
        RecordId record_id = id + project_record_length;
        project.types.push_back(CheckedType::Record(record_id));

        auto object_type_id = project.types.size() - 1;
        ErrorOr<Void> type_error = project.add_type_to_scope(
                scope_id,
                object->id.value,
                object_type_id,
                object->id.span
        );
        if (not type_error.has_value())
            error = error.value_or(type_error.error());
    }

    for (RecordId id = 0; id < parsed_namespace.objects.size(); id++) {
        auto object = parsed_namespace.objects[id];
        RecordId record_id = id + project_record_length;

        Opt<Error> x = typecheck_record_predecl(*object, record_id, scope_id, project);
        if (x.has_value())
            error = error.value_or(x.value());
    }

    for (RecordId id = 0; id < parsed_namespace.objects.size(); id++) {
        auto object = parsed_namespace.objects[id];
        RecordId record_id = id + project_record_length;

        Opt<Error> x = typecheck_record(*object, record_id, scope_id, project);
        if (x.has_value())
            error = error.value_or(x.value());
    }

    return error;
}

Opt<Error> typecheck_record_predecl(const ParsedObject& record, RecordId record_id, ScopeId parent_scope_id, Project& project) {
    Opt<Error> error = std::nullopt;

    TypeId type_id = project.find_or_add_type_id(CheckedType::Record(record_id));
    ScopeId scope_id = project.create_scope(parent_scope_id);

    Vec<TypeId> generic_parameters = {};
    for (auto generic_parameter : record.generic_params) {
        project.types.push_back(CheckedType::TypeVariable(generic_parameter->id.value));
        TypeId parameter_type_id = project.types.size() - 1;

        generic_parameters.push_back(parameter_type_id);

        ErrorOr<Void> x = project.add_type_to_scope(scope_id, generic_parameter->id.value, parameter_type_id, generic_parameter->id.span);
        if (not x.has_value())
            error = error.value_or(x.error());
    }

    for (const auto& method : record.methods) {
        Vec<TypeId> method_generic_parameters = {};
        ScopeId method_scope_id = project.create_scope(scope_id);

        // TODO: generic parameters for functions/methods
//        for (Type *generic_parameter : method.generic_parameters) {
//            project.types.push_back(CheckedType::TypeVariable(generic_parameter->id.value));
//            TypeId type_var_type_id = project.types.size() - 1;
//        }

        auto checked_function = CheckedFunction{
            .name = method.id.value,
            .return_type_id = UNKNOWN_TYPE_ID,
            .parameters = {},
            .generic_parameters = generic_parameters,
            .scope_id = method_scope_id,
        };

        for (const auto& parameter : method.parameters) {
            auto [param_type, err] = typecheck_typename(parameter.type, method_scope_id, project);
            if (err.has_value())
                error = error.value_or(err.value());

            auto checked_variable = CheckedVariable{
                .name = parameter.id.value,
                .type_id = param_type,
            };

            checked_function.parameters.push_back(CheckedParameter{
                .requires_label = false,
                .variable = checked_variable,
            });
        }

        project.functions.push_back(checked_function);
        ErrorOr<Void> x = project.add_function_to_scope(scope_id, method.id.value, project.functions.size() - 1, record.id.span);
        if (not x.has_value())
            error = error.value_or(x.error());
    }

    project.records.push_back(CheckedRecord{
        .name = record.id.value,
        .generic_parameters = generic_parameters,
        .fields = {},
        .scope_id = scope_id,
    });

    ErrorOr<Void> x = project.add_record_to_scope(parent_scope_id, record.id.value, record_id, record.id.span);
    if (not x.has_value())
        error = error.value_or(x.error());

    return error;
}

Opt<Error> typecheck_record(const ParsedObject& object, RecordId record_id, ScopeId parent_scope_id, Project& project) {
    Opt<Error> error = std::nullopt;

    Vec<CheckedVarDecl> fields = {};

    CheckedRecord checked_record = project.records[record_id];
    ScopeId checked_record_scope_id = checked_record.scope_id;
    TypeId record_type_id = project.find_or_add_type_id(CheckedType::Record(record_id));

    for (const auto& unchecked_member : object.fields) {
        auto [checked_member_type, err] = typecheck_typename(unchecked_member.type, checked_record_scope_id, project);
        if (err.has_value()) error = error.value_or(err.value());

        fields.push_back(CheckedVarDecl{
            .name = unchecked_member.id.value,
            .type_id = checked_member_type,
            .span = unchecked_member.id.span,
        });
    }

    // No constructor was found so we need to make one
    if (not project.find_function_in_scope(checked_record_scope_id, object.id.value).has_value()) {
        Vec<CheckedParameter> params = {};
        for (const auto& field : fields) {
            params.push_back(CheckedParameter{
                    .requires_label = true,
                    .variable = CheckedVariable{
                            .name = field.name,
                            .type_id = field.type_id,
                    }
            });

            ScopeId constructor_scope_id = project.create_scope(parent_scope_id);
            auto checked_constructor = CheckedFunction{
                    .name = object.id.value,
                    .return_type_id = record_type_id,
                    .parameters = params,
                    .generic_parameters = {},
                    .scope_id = constructor_scope_id,
            };

            project.functions.push_back(checked_constructor);

            auto x = project.add_function_to_scope(checked_record_scope_id, object.id.value,
                                                   project.functions.size() - 1, object.id.span);
            if (not x.has_value())
                error = error.value_or(x.error());
        }
    }

    CheckedRecord record = project.records[record_id];
    record.fields = fields;

    for (const auto& fn : object.methods) {
        Opt<Error> x = typecheck_method(fn, record_id, project);
        if (x.has_value()) error = error.value_or(x.value());
    }

    return error;
}


Opt<Error> typecheck_method(const ParsedMethod& method, RecordId record_id, Project& project) {
    Opt<Error> error = std::nullopt;

    CheckedRecord record = project.records[record_id];
    ScopeId record_scope_id = record.scope_id;

    Opt<FunctionId> opt_method_id = project.find_function_in_scope(record_scope_id, method.id.value);
    if (not opt_method_id.has_value()) PANIC("Internal error: pushed a checked function but it's not defined.");
    FunctionId method_id = opt_method_id.value();

    CheckedFunction checked_function = project.functions[method_id];
    ScopeId function_scope_id = checked_function.scope_id;

    Vec<CheckedVariable> parameters = {};
    for (const auto &parameter : checked_function.parameters) {
        parameters.push_back(parameter.variable);
    }

    for (const auto &variable : parameters) {
        ErrorOr<Void> x = project.add_var_to_scope(function_scope_id, variable, method.id.span);
        if (not x.has_value())
            error = error.value_or(x.error());
    }

    // TODO: typecheck_block

    TypeId return_type_id = UNKNOWN_TYPE_ID;
    if (method.ret_type.has_value()) {
        auto [function_return_type_id, err] = typecheck_typename(method.ret_type.value(), function_scope_id, project);
        if (err.has_value()) error = error.value_or(err.value());
        return_type_id = function_return_type_id;
    }

    if (return_type_id == UNKNOWN_TYPE_ID) return_type_id = UNIT_TYPE_ID;

    CheckedFunction checked_fn = project.functions[method_id];
//    checked_fn.block = block;
    checked_fn.return_type_id = return_type_id;

    return error;
}

std::tuple<CheckedStatement, Opt<Error>> typecheck_statement(ParsedStatement *statement, ScopeId scope_id, Project& project, SafetyContext context) {
    Opt<Error> error = std::nullopt;

    switch (static_cast<ParsedStatement::Kind>(statement->var.index())) {
        case ParsedStatement::Kind::Object:
        case ParsedStatement::Kind::Interface:
        case ParsedStatement::Kind::Fun:
        case ParsedStatement::Kind::Var:
        case ParsedStatement::Kind::Return:
        case ParsedStatement::Kind::Expr:
            UNIMPLEMENTED;
    }
}

std::tuple<CheckedExpression, Opt<Error>> typecheck_expression(Expression *expression, ScopeId scope_id, Project& project, SafetyContext context, Opt<TypeId> type_hint) {
    Opt<Error> error = std::nullopt;

    auto unify_with_type_hint = [&](Project &project, TypeId type_id) -> std::tuple<TypeId, Opt<Error>> {
        if (type_hint.has_value()) {
            TypeId hint = type_hint.value();
            if (hint == UNKNOWN_TYPE_ID)
                return std::make_tuple(type_id, std::nullopt);

            Map<TypeId, TypeId> generic_interface = {};
            Opt<Error> err = check_types_for_compat(
                    hint,
                    type_id,
                    &generic_interface,
                    expression->span(),
                    project
            );
            if (err.has_value())
                return std::make_tuple(type_id, err);

            return std::make_tuple(
                    substitute_typevars_in_type(type_id, &generic_interface, project),
                    std::nullopt
            );
        }

        return std::make_tuple(type_id, std::nullopt);
    };

    switch (static_cast<Expression::Kind>(expression->var.index())) {
        case Expression::Kind::Null:
        case Expression::Kind::Id:
        case Expression::Kind::Int:
        case Expression::Kind::String:
        case Expression::Kind::Call:
        case Expression::Kind::Index:
        case Expression::Kind::Generic:
        case Expression::Kind::Unary:
        case Expression::Kind::Binary:
        case Expression::Kind::If:
        case Expression::Kind::Access:
        case Expression::Kind::Switch:
        case Expression::Kind::UnsafeBlock:
            UNIMPLEMENTED;
    }
}

std::tuple<TypeId, Opt<Error>> typecheck_typename(Type *unchecked_type, ScopeId scope_id, Project& project) {
    Opt<Error> error = std::nullopt;

    switch (unchecked_type->type) {
        case Type::Kind::Undetermined: return std::make_tuple(UNKNOWN_TYPE_ID, std::nullopt);
        case Type::Kind::Id: {
            Opt<TypeId> type_id = project.find_type_in_scope(scope_id, unchecked_type->id.value);
            if (type_id.has_value())
                return std::make_tuple(type_id.value(), std::nullopt);
            else
                return std::make_tuple(UNKNOWN_TYPE_ID, std::make_optional(Error{"unknown type", unchecked_type->id.span}));
        }
        case Type::Kind::Str: return std::make_tuple(STRING_TYPE_ID, std::nullopt);
        case Type::Kind::Int: return std::make_tuple(INT_TYPE_ID, std::nullopt);
        case Type::Kind::Array: {
            auto [inner_type_id, err] = typecheck_typename(unchecked_type->subtype, scope_id, project);
            if (err.has_value()) error = error.value_or(err.value());

            Opt<RecordId> opt_array_record_id = project
                    .find_record_in_scope(0, "Array");
            if (not opt_array_record_id.has_value())
                PANIC("internal error: `Array` builtin was not found");
            RecordId array_record_id = opt_array_record_id.value();

            TypeId type_id = project.find_or_add_type_id(CheckedType::GenericInstance(array_record_id, {inner_type_id}));

            return std::make_tuple(type_id, error);
        }
        case Type::Kind::Weak: break;
        case Type::Kind::Raw: {
            auto [inner_type_id, err] = typecheck_typename(unchecked_type->subtype, scope_id, project);
            if (err.has_value()) error = error.value_or(err.value());

            TypeId type_id = project.find_or_add_type_id(CheckedType::RawPtr(inner_type_id));

            return std::make_tuple(type_id, error);
        }
        case Type::Kind::Optional: break;
        case Type::Kind::Generic: {
            Vec<TypeId> checked_inner_types = {};

            for (const auto& inner_type : unchecked_type->generic_args) {
                auto [inner_type_id, err] = typecheck_typename(inner_type, scope_id, project);
                if (err.has_value()) error = error.value_or(err.value());

                checked_inner_types.push_back(inner_type_id);
            }

            Opt<RecordId> record_id = project.find_record_in_scope(scope_id, unchecked_type->id.value);
            if (record_id.has_value())
                return std::make_tuple(project.find_or_add_type_id(CheckedType::GenericInstance(record_id.value(), checked_inner_types)), error);
            else return std::make_tuple(UNKNOWN_TYPE_ID, std::make_optional(Error{std::format("undefined type `{}`", unchecked_type->id.value), unchecked_type->id.span}));
        }
    }
}

TypeId substitute_typevars_in_type(TypeId type_id, Map<TypeId, TypeId> *generic_inferences, Project &project) {
    TypeId result = substitute_typevars_in_type_helper(type_id, generic_inferences, project);

    for (;;) {
        TypeId fixed_point = substitute_typevars_in_type_helper(type_id, generic_inferences, project);
        if (fixed_point == result) break;
        else result = fixed_point;
    }

    return result;
}

TypeId substitute_typevars_in_type_helper(TypeId type_id, Map<TypeId, TypeId> *generic_inferences, Project &project) {
    CheckedType type = project.types[type_id];
    switch (type.tag) {
        case CheckedType::Tag::TypeVariable:
            if (generic_inferences->contains(type_id))
                return generic_inferences->at(type_id);
            break;
        case CheckedType::Tag::GenericInstance: {
            RecordId record_id = type.generic_instance.record_id;
            Vec<TypeId> args = type.generic_instance.generic_arguments;

            for (TypeId& idx : args) {
                TypeId *arg = &idx;
                *arg = substitute_typevars_in_type(*arg, generic_inferences, project);
            }

            return project.find_or_add_type_id(CheckedType::GenericInstance(record_id, args));
        } break;
        case CheckedType::Tag::Record: {
            RecordId record_id = type.record.record_id;
            CheckedRecord record = project.records[record_id];

            if (not record.generic_parameters.empty()) {
                Vec<TypeId> args = record.generic_parameters;

                for (TypeId& idx : args) {
                    TypeId *arg = &idx;
                    *arg = substitute_typevars_in_type(*arg, generic_inferences, project);
                }

                return project.find_or_add_type_id(CheckedType::GenericInstance(record_id, args));
            }
        } break;
        default: break;
    }
    return type_id;
}

Opt<Error> check_types_for_compat(
        TypeId lhs_type_id,
        TypeId rhs_type_id,
        Map<TypeId, TypeId> *generic_inferences,
        Span span,
        Project &project
) {
    Opt<Error> error = std::nullopt;
    CheckedType lhs_type = project.types[lhs_type_id];

    Opt<RecordId> opt_optional_record_id = project.find_record_in_scope(0, "Optional");
    if (not opt_optional_record_id.has_value())
        PANIC("internal error: `Optional` builtin was not found");
    RecordId optional_record_id = opt_optional_record_id.value();

    Opt<RecordId> opt_weak_ptr_record_id = project.find_record_in_scope(0, "WeakPtr");
    if (not opt_weak_ptr_record_id.has_value())
        PANIC("internal error: `WeakPtr` builtin was not found");
    RecordId weak_ptr_record_id = opt_weak_ptr_record_id.value();

    if (lhs_type.tag == CheckedType::Tag::GenericInstance) {
        RecordId lhs_struct_id = lhs_type.generic_instance.record_id;
        Vec<TypeId> args = lhs_type.generic_instance.generic_arguments;

        if ((lhs_struct_id == optional_record_id or lhs_struct_id == weak_ptr_record_id) and args.front() == rhs_type_id) {
            return std::nullopt;
        }
    }

    switch (lhs_type.tag) {
        case CheckedType::Tag::TypeVariable: {
            if (generic_inferences->contains(lhs_type_id)) {
                TypeId seen_type_id = generic_inferences->at(lhs_type_id);

                if (rhs_type_id != seen_type_id) {
                    error = error.value_or(Error{
                            std::format("type mismatch; expected {}, but got {} instead",
                                        project.typename_for_type_id(seen_type_id),
                                        project.typename_for_type_id(rhs_type_id)),
                            span
                    });
                }
            } else {
                generic_inferences->insert({lhs_type_id, rhs_type_id});
            }
        } break;
        case CheckedType::Tag::GenericInstance: {
            RecordId lhs_record_id = lhs_type.generic_instance.record_id;
            Vec<TypeId> lhs_args = lhs_type.generic_instance.generic_arguments;
            CheckedType rhs_type = project.types[rhs_type_id];
            if (rhs_type.tag == CheckedType::Tag::GenericInstance) {
                RecordId rhs_record_id = rhs_type.generic_instance.record_id;
                if (lhs_record_id == rhs_record_id) {
                    Vec<TypeId> rhs_args = rhs_type.generic_instance.generic_arguments;

                    CheckedRecord lhs_record = project.records[lhs_record_id];
                    if (rhs_args.size() != lhs_args.size())
                        return Error{
                                std::format("mismatched number of generic parameters for {}", lhs_record.name),
                                span
                        };

                    for (usz idx = 0; idx < lhs_args.size(); idx++) {
                        TypeId lhs_arg_type_id = lhs_args[idx];
                        TypeId rhs_arg_type_id = rhs_args[idx];

                        Opt<Error> err = check_types_for_compat(lhs_arg_type_id, rhs_arg_type_id, generic_inferences, span, project);
                        if (err.has_value()) return err.value();
                    }
                }
            } else {
                if (rhs_type_id != lhs_type_id) {
                    error = error.value_or(Error{
                            std::format("type mismatch; expected {}, but got {} instead",
                                        project.typename_for_type_id(lhs_type_id),
                                        project.typename_for_type_id(rhs_type_id)),
                            span
                    });
                }
            }
        } break;
        case CheckedType::Tag::Record: {
            if (rhs_type_id == lhs_type_id) return std::nullopt;
            else error = error.value_or(Error{
                        std::format("type mismatch; expected {}, but got {} instead",
                                    project.typename_for_type_id(lhs_type_id),
                                    project.typename_for_type_id(rhs_type_id)),
                        span
                });
        } break;
        default:
            if (rhs_type_id != lhs_type_id)
                error = error.value_or(Error{
                        std::format("type mismatch; expected {}, but got {} instead",
                                    project.typename_for_type_id(lhs_type_id),
                                    project.typename_for_type_id(rhs_type_id)),
                        span
                });
            break;
    }

    return error;
}
