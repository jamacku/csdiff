/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This file is part of csdiff.
 *
 * csdiff is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * csdiff is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with csdiff.  If not, see <http://www.gnu.org/licenses/>.
 */

/* use C++ instead of C */
%option c++

/* count line numbers automatically */
%option yylineno

/* do not use yywrap function/method */
%option noyywrap

%{
#include "csparser-priv.hh"
%}

spaces              [ \t\n]*
nocolons            [^:]+

init                ^Error:{spaces}
defect              [A-Z][A-Z_]+:$
file                ^{nocolons}\/{nocolons}
line                :[0-9:]+{spaces}
mesg                [a-z][a-z_-]+:\ .*$
mesg_extra          ^\ +.*$

%%
"\n"
{init}              return T_INIT;
{defect}            return T_DEFECT;
{file}              return T_FILE;
{line}              return T_LINE;
{mesg}              return T_MSG;
{mesg_extra}        return T_MSG_EX;

%%
