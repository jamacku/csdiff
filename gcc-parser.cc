/*
 * Copyright (C) 2013 Red Hat, Inc.
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

#include "gcc-parser.hh"

#include <algorithm>

#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>

enum EToken {
    T_NULL = 0,
    T_UNKNOWN,

    T_INC,
    T_SCOPE,
    T_MSG,
    T_MARKER
};

class ITokenizer {
    public:
        virtual ~ITokenizer() { }
        virtual EToken readNext(DefEvent *pEvt) = 0;
        virtual int lineNo() const = 0;
};

class AbstractTokenFilter: public ITokenizer {
    public:
        virtual int lineNo() const {
            return slave_->lineNo();
        }

    protected:
        /// @param slave the object will NOT be deleted on destruction
        AbstractTokenFilter(ITokenizer *slave):
            slave_(slave)
        {
        }

        ITokenizer *slave_;
};

#define RE_LOCATION "([^:]+)(?::([0-9]+))?(?::([0-9]+))?"

class Tokenizer: public ITokenizer {
    public:
        Tokenizer(std::istream &input):
            input_(input),
            lineNo_(0),
            reMarker_("^ *\\^$"),
            reInc_("^(?:In file included| +) from " RE_LOCATION "[:,]"),
            reScope_("^" RE_LOCATION ": ([A-Z].+):$"),
            reMsg_("^" RE_LOCATION /* evt/mesg */ ": ([a-z]+): (.*)$")
        {
        }

        virtual int lineNo() const {
            return lineNo_;
        }

        virtual EToken readNext(DefEvent *pEvt);

    private:
        std::istream           &input_;
        int                     lineNo_;
        const boost::regex      reMarker_;
        const boost::regex      reInc_;
        const boost::regex      reScope_;
        const boost::regex      reMsg_;
};

EToken Tokenizer::readNext(DefEvent *pEvt) {
    std::string line;
    if (!std::getline(input_, line))
        return T_NULL;

    lineNo_++;

    if (boost::regex_match(line, reMarker_))
        return T_MARKER;

    EToken tok;
    boost::smatch sm;

    if (boost::regex_match(line, sm, reMsg_)) {
        tok = T_MSG;
        pEvt->event = sm[/* evt  */ 4];
        pEvt->msg   = sm[/* msg  */ 5];
    }
    else if (boost::regex_match(line, sm, reScope_)) {
        tok = T_SCOPE;
        pEvt->event = "scope_hint";
        pEvt->msg   = sm[/* msg  */ 4];
    }
    else if (boost::regex_match(line, sm, reInc_)) {
        tok = T_INC;
        pEvt->event = "included_from";
        pEvt->msg   = "Included from here.";
    }
    else
        return T_UNKNOWN;

    // read file name, event, and msg
    pEvt->fileName    = sm[/* file */ 1];

    // parse line number
    try {
        pEvt->line = boost::lexical_cast<int>(sm[/* line */ 2]);
    }
    catch (boost::bad_lexical_cast &) {
        pEvt->line = 0;
    }

    // parse column number
    try {
        pEvt->column = boost::lexical_cast<int>(sm[/* col */ 3]);
    }
    catch (boost::bad_lexical_cast &) {
        pEvt->column = 0;
    }

    return tok;
}

class MarkerRemover: public AbstractTokenFilter {
    public:
        MarkerRemover(ITokenizer *slave):
            AbstractTokenFilter(slave)
        {
        }

        virtual EToken readNext(DefEvent *pEvt);
};

EToken MarkerRemover::readNext(DefEvent *pEvt) {
    EToken tok = slave_->readNext(pEvt);
    if (T_UNKNOWN != tok)
        return tok;

    tok = slave_->readNext(pEvt);
    if (T_MARKER != tok)
        return tok;

    return slave_->readNext(pEvt);
}

class MultilineConcatenator: public AbstractTokenFilter {
    public:
        MultilineConcatenator(ITokenizer *slave):
            AbstractTokenFilter(slave),
            lastTok_(T_NULL),
            reBase_("^([^ ].+)( \\[[^\\]]+\\])$"),
            reExtra_("^ *( [^ ].+)( \\[[^\\]]+\\])$")
        {
        }

        virtual EToken readNext(DefEvent *pEvt);

    private:
        EToken                  lastTok_;
        DefEvent                lastEvt_;
        const boost::regex      reBase_;
        const boost::regex      reExtra_;

        bool tryMerge(DefEvent *pEvt);
};

bool MultilineConcatenator::tryMerge(DefEvent *pEvt) {
    if (T_MSG != lastTok_)
        // only messages can be merged together
        return false;

    if (pEvt->event != lastEvt_.event)
        return false;

    // TODO: compare also the location info?

    boost::smatch smBase;
    if (!boost::regex_match(pEvt->msg, smBase, reBase_))
        return false;

    boost::smatch smExtra;
    if (!boost::regex_match(lastEvt_.msg, smExtra, reExtra_))
        return false;

    // we need to drop the [-Wreason] suffix from the first message if same
    if (smBase[/* -W suffix */ 2] != smExtra[/* -W suffix */ 2])
        return false;

    // concatenate both messages together
    pEvt->msg = smBase[/* msg */ 1] + smExtra[/* msg */1] + smExtra[/* suf */2];

    // clear the already merged token
    lastTok_ = T_NULL;
    return true;
}

EToken MultilineConcatenator::readNext(DefEvent *pEvt) {
    EToken tok;
    switch (lastTok_) {
        case T_NULL:
            // no last token --> we have to read a new one
            tok = slave_->readNext(pEvt);
            break;

        case T_MSG:
            // reuse the last T_MSG token
            tok = lastTok_;
            *pEvt = lastEvt_;
            break;

        default:
            // flush the last token and bail out
            tok = lastTok_;
            *pEvt = lastEvt_;
            lastTok_ = T_NULL;
            return tok;
    }

    if (T_MSG == tok) {
        do
            // read one token ahead
            lastTok_ = slave_->readNext(&lastEvt_);

        while
            // try to merge it with the previous one
            (this->tryMerge(pEvt));
    }

    return tok;
}

class BasicGccParser {
    public:
        BasicGccParser(
                std::istream       &input,
                const std::string  &fileName,
                const bool          silent):
            rawTokenizer_(input),
            markerRemover_(&rawTokenizer_),
            tokenizer_(&markerRemover_),
            fileName_(fileName),
            silent_(silent),
            reChecker_("^([A-Za-z]+): (.*)$"),
            hasKeyEvent_(false),
            hasError_(false)
        {
        }

        bool getNext(Defect *);
        bool hasError() const;

    private:
        Tokenizer               rawTokenizer_;
        MarkerRemover           markerRemover_;
        MultilineConcatenator   tokenizer_;
        const std::string       fileName_;
        const bool              silent_;
        const boost::regex      reChecker_;
        bool                    hasKeyEvent_;
        bool                    hasError_;
        Defect                  defCurrent_;

        void handleError();
        bool exportAndReset(Defect *pDef);
};

void BasicGccParser::handleError() {
    hasError_ = true;
    if (silent_)
        return;

    std::cerr << fileName_ << ":" << tokenizer_.lineNo()
        << ": error: invalid syntax\n";
}

bool BasicGccParser::exportAndReset(Defect *pDef) {
    Defect &def = defCurrent_;
    if (def.events.empty())
        return false;

    if (!hasKeyEvent_) {
        this->handleError();
        return false;
    }

    DefEvent &evt = def.events[def.keyEventIdx];

    // use cppcheck's ID as the checker string if available
    boost::smatch sm;
    if (boost::regex_match(evt.msg, sm, reChecker_)) {
        def.checker = sm[/* id  */ 1];
        evt.msg     = sm[/* msg */ 2];
    }
    else
        def.checker = "COMPILER_WARNING";

    // export the current state and clear the data for next iteration
    *pDef = def;
    def = Defect();
    hasKeyEvent_ = false;
    return true;
}

bool BasicGccParser::getNext(Defect *pDef) {
    bool done = false;
    while (!done) {
        DefEvent evt;

        const EToken tok = tokenizer_.readNext(&evt);
        switch (tok) {
            case T_NULL:
                return this->exportAndReset(pDef);

            case T_INC:
            case T_SCOPE:
                done = hasKeyEvent_ && this->exportAndReset(pDef);
                defCurrent_.events.push_back(evt);
                break;

            case T_MSG:
                done = hasKeyEvent_ && this->exportAndReset(pDef);
                defCurrent_.keyEventIdx = defCurrent_.events.size();
                defCurrent_.events.push_back(evt);
                hasKeyEvent_ = true;
                break;

            case T_MARKER:
            case T_UNKNOWN:
                this->handleError();
                continue;
        }
    }

    return true;
}

bool BasicGccParser::hasError() const {
    return hasError_;
}

struct GccParser::Private {
    BasicGccParser              core;
    Defect                      lastDef;

    Private(
            std::istream       &input_,
            const std::string  &fileName_,
            const bool          silent_):
        core(input_, fileName_, silent_)
    {
    }

    bool tryMerge(Defect *pDef);
};

GccParser::GccParser(
        std::istream           &input,
        const std::string      &fileName,
        const bool              silent):
    d(new Private(input, fileName, silent))
{
}

GccParser::~GccParser() {
    delete d;
}

bool GccParser::Private::tryMerge(Defect *pDef) {
    if (pDef->checker != this->lastDef.checker)
        return false;

    TEvtList &lastEvts = this->lastDef.events;
    const DefEvent &lastKeyEvt = lastEvts[this->lastDef.keyEventIdx];
    if (lastKeyEvt.event != "note")
        // we try to merge only "note" events for now
        return false;

    TEvtList &evts = pDef->events;
    const DefEvent &keyEvt = evts[pDef->keyEventIdx];
    if (keyEvt.event == "note")
        // avoid using "note" as the key event
        return false;

    // concatenate the events and purge the last defect
    std::copy(lastEvts.begin(), lastEvts.end(), std::back_inserter(evts));
    lastEvts.clear();
    return true;
}

bool GccParser::getNext(Defect *pDef) {
    // pick the last defect and clear the stash
    *pDef = d->lastDef;
    d->lastDef.events.clear();
    if (pDef->events.size() <= pDef->keyEventIdx
        // no valid last defect --> read a new one
            && !d->core.getNext(pDef))
        // no valid current defect either
        return false;

    // defect merging loop
    while (d->core.getNext(&d->lastDef) && d->tryMerge(pDef))
        ;

    return true;
}

bool GccParser::hasError() const {
    return d->core.hasError();
}