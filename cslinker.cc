/*
 * Copyright (C) 2012 Red Hat, Inc.
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

#include "abstract-parser.hh"
#include "defqueue.hh"
#include "instream.hh"
#include "json-writer.hh"

#include <boost/program_options.hpp>

bool loadPropsFronIniFile(
        AbstractWriter             &writer,
        std::istream               &input,
        const std::string          &fName)
{
    // TODO
    (void) writer;
    (void) input;
    (void) fName;
    return false;
}

class OrphanWriter {
    public:
        OrphanWriter(AbstractWriter &writer):
            writer_(writer)
        {
        }

        bool operator()(const Defect &def) {
            writer_.handleDef(def);
            return /* continue */ true;
        }

    private:
        AbstractWriter &writer_;
};

bool writeMappedDefects(
        AbstractWriter             &writer,
        DefQueue                   &defQueue,
        std::istream               &input,
        const std::string          &fName)
{
    DefQueryParser qParser(input, fName);
    DefQueryParser::QRow row;
    while (qParser.getNext(row)) {
        Defect def;
        if (!defQueue.lookup(def, row.checker, row.fileName)) {
            std::cerr
                << fName << ": warning: defect lookup failed, cid = "
                << row.cid << "\n";
        }

        def.defectId = row.cid;
        def.function = row.fnc;
        writer.handleDef(def);
    }

    OrphanWriter visitor(writer);
    return defQueue.walk(visitor);
}

int main(int argc, char *argv[])
{
    using std::string;
    const char *name = argv[0];

    namespace po = boost::program_options;
    po::variables_map vm;
    po::options_description desc(string("Usage: ") + name
            + " [options] proj.err [proj.ini [proj.map]], where options are");

    typedef std::vector<string> TStringList;
    string mode;

    try {
        desc.add_options()
            ("quiet,q", "do not report any parsing errors")
            ("help", "produce help message");

        po::options_description hidden("");
        hidden.add_options()
            ("input-file", po::value<TStringList>(), "input file");
        po::positional_options_description p;
        p.add("input-file", -1);

        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);    

        po::options_description opts;
        opts.add(desc).add(hidden);
        po::store(po::command_line_parser(argc, argv).
                options(opts).positional(p).run(), vm);
        po::notify(vm);
    }
    catch (po::error &e) {
        std::cerr << name << ": error: " << e.what() << "\n\n";
        desc.print(std::cerr);
        return 1;
    }

    if (vm.count("help")) {
        desc.print(std::cout);
        return 0;
    }

    if (!vm.count("input-file")) {
        desc.print(std::cerr);
        return 1;
    }

    TStringList files = vm["input-file"].as<TStringList>();
    if (3 < files.size() || files.size() < 1) {
        desc.print(std::cerr);
        return 1;
    }

    files.resize(3, string());
    const string &fnErr = files[0];
    const string &fnIni = files[1];
    const string &fnMap = files[2];

    const bool silent = vm.count("quiet");

    try {
        // initialize parser for .err
        InStream strErr(fnErr.c_str());
        Parser pErr(strErr.str(), fnErr, silent);

        // initialize JSON writer
        JsonWriter writer;
        writer.setScanProps(pErr.getScanProps());

        bool hasError = false;

        // load .ini if available
        if (!fnIni.empty()) {
            InStream strIni(fnIni.c_str());
            if (!loadPropsFronIniFile(writer, strIni.str(), fnIni))
                hasError = true;
        }

        if (fnMap.empty())
            // no .map file given
            writer.handleFile(pErr, fnErr);

        else {
            // open the given .map file
            InStream strMap(fnMap.c_str());

            // load defects from .err
            DefQueue defQueue;
            Defect def;
            while (pErr.getNext(&def))
                defQueue.hashDefect(def);

            // process the given .map file
            if (!writeMappedDefects(writer, defQueue, strMap.str(), fnMap))
                hasError = true;
        }

        writer.flush();

        return hasError
            || pErr.hasError();
    }
    catch (const InFileException &e) {
        std::cerr << e.fileName << ": failed to open input file\n";
        return EXIT_FAILURE;
    }
}