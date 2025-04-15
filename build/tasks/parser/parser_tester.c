#include <check.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "parser.h"

START_TEST (test_parse_name_only)
{
    char * program_name = "programa";
    argument_list * l = parse_arguments(program_name);
    fail_unless(l != NULL, get_parse_error());
    fail_unless(l->length == 1,"Se esperaba %d se obtuvo %d\n",1,l->length);
    fail_unless(!strcmp(l->list[0].str,program_name));
} END_TEST

START_TEST (test_parse_arguments)
{
    char * program = "programa arg1 arg2 arg3";
    argument_list * l = parse_arguments(program);
    fail_unless(l != NULL, get_parse_error());
    fail_unless(l->length == 4);
    fail_unless(!strcmp(l->list[0].str,"programa"));
    fail_unless(!strcmp(l->list[1].str,"arg1"));
    fail_unless(!strcmp(l->list[2].str,"arg2"));
    fail_unless(!strcmp(l->list[3].str,"arg3"));
}
END_TEST

START_TEST (test_too_long_argument)
{
    char * program = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    argument_list * l = parse_arguments(program);
    fail_unless(l == NULL);
    fail_unless(!strcmp(get_parse_error(),
                        "Argumento demasiado largo"));
}
END_TEST

START_TEST (test_too_many_arguments)
{
    char * program = "a a a a a a a a a a a a a a a a a a a "
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a"
                     "a a a a a a a a a a a a a a a a a a a a a a a a a a a a";

    argument_list * l = parse_arguments(program);
    fail_unless(l == NULL);
    fail_unless(!strcmp(get_parse_error(),
                        "Demasiados argumentos"));
}
END_TEST

START_TEST (test_incorrect_argument)
{
    char * program = "123 ¬¬°° jpdohsiii";

    argument_list * l = parse_arguments(program);
    fail_unless(l == NULL);
    fail_unless(!strcmp(get_parse_error(),
                        "Caracter invalido"));
}
END_TEST

START_TEST (test_empty_argument)
{
    char * program = "";

    argument_list * l = parse_arguments(program);
    fail_unless(l->length == 0);
}
END_TEST

START_TEST (test_empty_argument_except_spaces)
{
    char * program = "     ";

    argument_list * l = parse_arguments(program);
    fail_unless(l->length == 0);
}
END_TEST

START_TEST (test_filename)
{
    char * program = "cat /docs/prueba1.txt";
    argument_list * l = parse_arguments(program);
    fail_unless(l && l->length == 2);
    fail_unless(!strcmp(l->list[0].str,"cat"));
    fail_unless(!strcmp(l->list[1].str,"/docs/prueba1.txt"));
}
END_TEST

TFun tests[] = {
    test_parse_name_only,
    test_parse_arguments,
    test_too_long_argument,
    test_too_many_arguments,
    test_incorrect_argument,
    test_empty_argument,
    test_empty_argument_except_spaces,
    test_filename,
    NULL
};

int main()
{
    Suite * s = suite_create("Parser de shell");

    TCase * tc_core = tcase_create("Core");
    for(int i = 0; tests[i]; i++)
        tcase_add_test(tc_core,tests[i]);
    suite_add_tcase(s,tc_core);
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr,CK_VERBOSE);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
