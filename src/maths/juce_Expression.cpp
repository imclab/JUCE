/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-10 by Raw Material Software Ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the GNU General
   Public License (Version 2), as published by the Free Software Foundation.
   A copy of the license is included in the JUCE distribution, or can be found
   online at www.gnu.org/licenses.

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.rawmaterialsoftware.com/juce for more information.

  ==============================================================================
*/

#include "../core/juce_StandardHeader.h"

BEGIN_JUCE_NAMESPACE

#include "juce_Expression.h"
#include "../containers/juce_ReferenceCountedArray.h"


//==============================================================================
class Expression::Term  : public ReferenceCountedObject
{
public:
    Term() {}
    virtual ~Term() {}

    virtual Type getType() const throw() = 0;
    virtual Term* clone() const = 0;
    virtual const ReferenceCountedObjectPtr<Term> resolve (const Scope&, int recursionDepth) = 0;
    virtual const String toString() const = 0;
    virtual double toDouble() const                                          { return 0; }
    virtual int getInputIndexFor (const Term*) const                         { return -1; }
    virtual int getOperatorPrecedence() const                                { return 0; }
    virtual int getNumInputs() const                                         { return 0; }
    virtual Term* getInput (int) const                                       { return 0; }
    virtual const ReferenceCountedObjectPtr<Term> negated();

    virtual const ReferenceCountedObjectPtr<Term> createTermToEvaluateInput (const Scope&, const Term* /*inputTerm*/,
                                                                             double /*overallTarget*/, Term* /*topLevelTerm*/) const
    {
        jassertfalse;
        return 0;
    }

    virtual const String getName() const
    {
        jassertfalse; // You shouldn't call this for an expression that's not actually a function!
        return String::empty;
    }

    virtual void renameSymbol (const Symbol& oldSymbol, const String& newName, const Scope& scope, int recursionDepth)
    {
        for (int i = getNumInputs(); --i >= 0;)
            getInput (i)->renameSymbol (oldSymbol, newName, scope, recursionDepth);
    }

    class SymbolVisitor
    {
    public:
        virtual ~SymbolVisitor() {}
        virtual void useSymbol (const Symbol&) = 0;
    };

    virtual void visitAllSymbols (SymbolVisitor& visitor, const Scope& scope, int recursionDepth)
    {
        for (int i = getNumInputs(); --i >= 0;)
            getInput(i)->visitAllSymbols (visitor, scope, recursionDepth);
    }

private:
    JUCE_DECLARE_NON_COPYABLE (Term);
};


//==============================================================================
class Expression::Helpers
{
public:
    typedef ReferenceCountedObjectPtr<Term> TermPtr;

    // This helper function is needed to work around VC6 scoping bugs
    static inline const TermPtr& getTermFor (const Expression& exp) throw()       { return exp.term; }

    static void checkRecursionDepth (const int depth)
    {
        if (depth > 256)
            throw EvaluationError ("Recursive symbol references");
    }

    friend class Expression::Term; // (also only needed as a VC6 workaround)

    //==============================================================================
    /** An exception that can be thrown by Expression::evaluate(). */
    class EvaluationError  : public std::exception
    {
    public:
        EvaluationError (const String& description_)
            : description (description_)
        {
            DBG ("Expression::EvaluationError: " + description);
        }

        String description;
    };

    //==============================================================================
    class Constant  : public Term
    {
    public:
        Constant (const double value_, const bool isResolutionTarget_)
            : value (value_), isResolutionTarget (isResolutionTarget_) {}

        Type getType() const throw()                 { return constantType; }
        Term* clone() const                          { return new Constant (value, isResolutionTarget); }
        const TermPtr resolve (const Scope&, int)    { return this; }
        double toDouble() const                      { return value; }
        const TermPtr negated()                      { return new Constant (-value, isResolutionTarget); }

        const String toString() const
        {
            String s (value);
            if (isResolutionTarget)
                s = "@" + s;

            return s;
        }

        double value;
        bool isResolutionTarget;
    };

    //==============================================================================
    class BinaryTerm  : public Term
    {
    public:
        BinaryTerm (Term* const left_, Term* const right_) : left (left_), right (right_)
        {
            jassert (left_ != 0 && right_ != 0);
        }

        int getInputIndexFor (const Term* possibleInput) const
        {
            return possibleInput == left ? 0 : (possibleInput == right ? 1 : -1);
        }

        Type getType() const throw()        { return operatorType; }
        int getNumInputs() const            { return 2; }
        Term* getInput (int index) const    { return index == 0 ? left.getObject() : (index == 1 ? right.getObject() : 0); }

        virtual double performFunction (double left, double right) const = 0;
        virtual void writeOperator (String& dest) const = 0;

        const TermPtr resolve (const Scope& scope, int recursionDepth)
        {
            return new Constant (performFunction (left->resolve (scope, recursionDepth)->toDouble(),
                                                  right->resolve (scope, recursionDepth)->toDouble()), false);
        }

        const String toString() const
        {
            String s;

            const int ourPrecendence = getOperatorPrecedence();
            if (left->getOperatorPrecedence() > ourPrecendence)
                s << '(' << left->toString() << ')';
            else
                s = left->toString();

            writeOperator (s);

            if (right->getOperatorPrecedence() >= ourPrecendence)
                s << '(' << right->toString() << ')';
            else
                s << right->toString();

            return s;
        }

    protected:
        const TermPtr left, right;

        const TermPtr createDestinationTerm (const Scope& scope, const Term* input, double overallTarget, Term* topLevelTerm) const
        {
            jassert (input == left || input == right);
            if (input != left && input != right)
                return 0;

            const Term* const dest = findDestinationFor (topLevelTerm, this);

            if (dest == 0)
                return new Constant (overallTarget, false);

            return dest->createTermToEvaluateInput (scope, this, overallTarget, topLevelTerm);
        }
    };

    //==============================================================================
    class SymbolTerm  : public Term
    {
    public:
        explicit SymbolTerm (const String& symbol_) : symbol (symbol_) {}

        const TermPtr resolve (const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);
            return getTermFor (scope.getSymbolValue (symbol))->resolve (scope, recursionDepth + 1);
        }

        Type getType() const throw()    { return symbolType; }
        Term* clone() const             { return new SymbolTerm (symbol); }
        const String toString() const   { return symbol; }
        const String getName() const    { return symbol; }

        void visitAllSymbols (SymbolVisitor& visitor, const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);
            visitor.useSymbol (Symbol (scope.getScopeUID(), symbol));
            getTermFor (scope.getSymbolValue (symbol))->visitAllSymbols (visitor, scope, recursionDepth + 1);
        }

        void renameSymbol (const Symbol& oldSymbol, const String& newName, const Scope& scope, int /*recursionDepth*/)
        {
            if (oldSymbol.symbolName == symbol && scope.getScopeUID() == oldSymbol.scopeUID)
                symbol = newName;
        }

        String symbol;
    };

    //==============================================================================
    class Function  : public Term
    {
    public:
        explicit Function (const String& functionName_)  : functionName (functionName_) {}

        Function (const String& functionName_, const Array<Expression>& parameters_)
            : functionName (functionName_), parameters (parameters_)
        {}

        Type getType() const throw()    { return functionType; }
        Term* clone() const             { return new Function (functionName, parameters); }
        int getNumInputs() const        { return parameters.size(); }
        Term* getInput (int i) const    { return getTermFor (parameters [i]); }
        const String getName() const    { return functionName; }

        const TermPtr resolve (const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);
            double result = 0;
            const int numParams = parameters.size();
            if (numParams > 0)
            {
                HeapBlock<double> params (numParams);
                for (int i = 0; i < numParams; ++i)
                    params[i] = getTermFor (parameters.getReference(i))->resolve (scope, recursionDepth + 1)->toDouble();

                result = scope.evaluateFunction (functionName, params, numParams);
            }
            else
            {
                result = scope.evaluateFunction (functionName, 0, 0);
            }

            return new Constant (result, false);
        }

        int getInputIndexFor (const Term* possibleInput) const
        {
            for (int i = 0; i < parameters.size(); ++i)
                if (getTermFor (parameters.getReference(i)) == possibleInput)
                    return i;

            return -1;
        }

        const String toString() const
        {
            if (parameters.size() == 0)
                return functionName + "()";

            String s (functionName + " (");

            for (int i = 0; i < parameters.size(); ++i)
            {
                s << getTermFor (parameters.getReference(i))->toString();

                if (i < parameters.size() - 1)
                    s << ", ";
            }

            s << ')';
            return s;
        }

        const String functionName;
        Array<Expression> parameters;
    };

    //==============================================================================
    class DotOperator  : public BinaryTerm
    {
    public:
        DotOperator (SymbolTerm* const left_, Term* const right_)  : BinaryTerm (left_, right_) {}

        const TermPtr resolve (const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);

            EvaluationVisitor visitor (right, recursionDepth + 1);
            scope.visitRelativeScope (getSymbol()->symbol, visitor);
            return visitor.output;
        }

        Term* clone() const                             { return new DotOperator (getSymbol(), right); }
        const String getName() const                    { return "."; }
        int getOperatorPrecedence() const               { return 1; }
        void writeOperator (String& dest) const         { dest << '.'; }
        double performFunction (double, double) const   { return 0.0; }

        void visitAllSymbols (SymbolVisitor& visitor, const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);
            visitor.useSymbol (Symbol (scope.getScopeUID(), getSymbol()->symbol));

            SymbolVisitingVisitor v (right, visitor, recursionDepth + 1);

            try
            {
                scope.visitRelativeScope (getSymbol()->symbol, v);
            }
            catch (...) {}
        }

        void renameSymbol (const Symbol& oldSymbol, const String& newName, const Scope& scope, int recursionDepth)
        {
            checkRecursionDepth (recursionDepth);
            getSymbol()->renameSymbol (oldSymbol, newName, scope, recursionDepth);

            SymbolRenamingVisitor visitor (right, oldSymbol, newName, recursionDepth + 1);

            try
            {
                scope.visitRelativeScope (getSymbol()->symbol, visitor);
            }
            catch (...) {}
        }

    private:
        //==============================================================================
        class EvaluationVisitor  : public Scope::Visitor
        {
        public:
            EvaluationVisitor (const TermPtr& input_, const int recursionCount_)
                : input (input_), output (input_), recursionCount (recursionCount_) {}

            void visit (const Scope& scope)   { output = input->resolve (scope, recursionCount); }

            const TermPtr input;
            TermPtr output;
            const int recursionCount;

        private:
            JUCE_DECLARE_NON_COPYABLE (EvaluationVisitor);
        };

        class SymbolVisitingVisitor  : public Scope::Visitor
        {
        public:
            SymbolVisitingVisitor (const TermPtr& input_, SymbolVisitor& visitor_, const int recursionCount_)
                : input (input_), visitor (visitor_), recursionCount (recursionCount_) {}

            void visit (const Scope& scope)   { input->visitAllSymbols (visitor, scope, recursionCount); }

        private:
            const TermPtr input;
            SymbolVisitor& visitor;
            const int recursionCount;

            JUCE_DECLARE_NON_COPYABLE (SymbolVisitingVisitor);
        };

        class SymbolRenamingVisitor   : public Scope::Visitor
        {
        public:
            SymbolRenamingVisitor (const TermPtr& input_, const Expression::Symbol& symbol_, const String& newName_, const int recursionCount_)
                : input (input_), symbol (symbol_), newName (newName_), recursionCount (recursionCount_)  {}

            void visit (const Scope& scope)   { input->renameSymbol (symbol, newName, scope, recursionCount); }

        private:
            const TermPtr input;
            const Symbol& symbol;
            const String newName;
            const int recursionCount;

            JUCE_DECLARE_NON_COPYABLE (SymbolRenamingVisitor);
        };

        SymbolTerm* getSymbol() const  { return static_cast <SymbolTerm*> (left.getObject()); }

        JUCE_DECLARE_NON_COPYABLE (DotOperator);
    };

    //==============================================================================
    class Negate  : public Term
    {
    public:
        explicit Negate (const TermPtr& input_) : input (input_)
        {
            jassert (input_ != 0);
        }

        Type getType() const throw()                            { return operatorType; }
        int getInputIndexFor (const Term* possibleInput) const  { return possibleInput == input ? 0 : -1; }
        int getNumInputs() const                                { return 1; }
        Term* getInput (int index) const                        { return index == 0 ? input.getObject() : 0; }
        Term* clone() const                                     { return new Negate (input->clone()); }

        const TermPtr resolve (const Scope& scope, int recursionDepth)
        {
            return new Constant (-input->resolve (scope, recursionDepth)->toDouble(), false);
        }

        const String getName() const    { return "-"; }
        const TermPtr negated()         { return input; }

        const TermPtr createTermToEvaluateInput (const Scope& scope, const Term* input_, double overallTarget, Term* topLevelTerm) const
        {
            (void) input_;
            jassert (input_ == input);

            const Term* const dest = findDestinationFor (topLevelTerm, this);

            return new Negate (dest == 0 ? new Constant (overallTarget, false)
                                         : dest->createTermToEvaluateInput (scope, this, overallTarget, topLevelTerm));
        }

        const String toString() const
        {
            if (input->getOperatorPrecedence() > 0)
                return "-(" + input->toString() + ")";
            else
                return "-" + input->toString();
        }

    private:
        const TermPtr input;
    };

    //==============================================================================
    class Add  : public BinaryTerm
    {
    public:
        Add (Term* const left_, Term* const right_) : BinaryTerm (left_, right_) {}

        Term* clone() const                     { return new Add (left->clone(), right->clone()); }
        double performFunction (double lhs, double rhs) const    { return lhs + rhs; }
        int getOperatorPrecedence() const       { return 3; }
        const String getName() const            { return "+"; }
        void writeOperator (String& dest) const { dest << " + "; }

        const TermPtr createTermToEvaluateInput (const Scope& scope, const Term* input, double overallTarget, Term* topLevelTerm) const
        {
            const TermPtr newDest (createDestinationTerm (scope, input, overallTarget, topLevelTerm));
            if (newDest == 0)
                return 0;

            return new Subtract (newDest, (input == left ? right : left)->clone());
        }

    private:
        JUCE_DECLARE_NON_COPYABLE (Add);
    };

    //==============================================================================
    class Subtract  : public BinaryTerm
    {
    public:
        Subtract (Term* const left_, Term* const right_) : BinaryTerm (left_, right_) {}

        Term* clone() const                     { return new Subtract (left->clone(), right->clone()); }
        double performFunction (double lhs, double rhs) const    { return lhs - rhs; }
        int getOperatorPrecedence() const       { return 3; }
        const String getName() const            { return "-"; }
        void writeOperator (String& dest) const { dest << " - "; }

        const TermPtr createTermToEvaluateInput (const Scope& scope, const Term* input, double overallTarget, Term* topLevelTerm) const
        {
            const TermPtr newDest (createDestinationTerm (scope, input, overallTarget, topLevelTerm));
            if (newDest == 0)
                return 0;

            if (input == left)
                return new Add (newDest, right->clone());
            else
                return new Subtract (left->clone(), newDest);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE (Subtract);
    };

    //==============================================================================
    class Multiply  : public BinaryTerm
    {
    public:
        Multiply (Term* const left_, Term* const right_) : BinaryTerm (left_, right_) {}

        Term* clone() const                     { return new Multiply (left->clone(), right->clone()); }
        double performFunction (double lhs, double rhs) const    { return lhs * rhs; }
        const String getName() const            { return "*"; }
        void writeOperator (String& dest) const { dest << " * "; }
        int getOperatorPrecedence() const       { return 2; }

        const TermPtr createTermToEvaluateInput (const Scope& scope, const Term* input, double overallTarget, Term* topLevelTerm) const
        {
            const TermPtr newDest (createDestinationTerm (scope, input, overallTarget, topLevelTerm));
            if (newDest == 0)
                return 0;

            return new Divide (newDest, (input == left ? right : left)->clone());
        }

    private:
        JUCE_DECLARE_NON_COPYABLE (Multiply);
    };

    //==============================================================================
    class Divide  : public BinaryTerm
    {
    public:
        Divide (Term* const left_, Term* const right_) : BinaryTerm (left_, right_) {}

        Term* clone() const                     { return new Divide (left->clone(), right->clone()); }
        double performFunction (double lhs, double rhs) const    { return lhs / rhs; }
        const String getName() const            { return "/"; }
        void writeOperator (String& dest) const { dest << " / "; }
        int getOperatorPrecedence() const       { return 2; }

        const TermPtr createTermToEvaluateInput (const Scope& scope, const Term* input, double overallTarget, Term* topLevelTerm) const
        {
            const TermPtr newDest (createDestinationTerm (scope, input, overallTarget, topLevelTerm));
            if (newDest == 0)
                return 0;

            if (input == left)
                return new Multiply (newDest, right->clone());
            else
                return new Divide (left->clone(), newDest);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE (Divide);
    };

    //==============================================================================
    static Term* findDestinationFor (Term* const topLevel, const Term* const inputTerm)
    {
        const int inputIndex = topLevel->getInputIndexFor (inputTerm);
        if (inputIndex >= 0)
            return topLevel;

        for (int i = topLevel->getNumInputs(); --i >= 0;)
        {
            Term* const t = findDestinationFor (topLevel->getInput (i), inputTerm);

            if (t != 0)
                return t;
        }

        return 0;
    }

    static Constant* findTermToAdjust (Term* const term, const bool mustBeFlagged)
    {
        {
            Constant* const c = dynamic_cast<Constant*> (term);
            if (c != 0 && (c->isResolutionTarget || ! mustBeFlagged))
                return c;
        }

        if (dynamic_cast<Function*> (term) != 0)
            return 0;

        int i;
        const int numIns = term->getNumInputs();
        for (i = 0; i < numIns; ++i)
        {
            Constant* const c = dynamic_cast<Constant*> (term->getInput (i));
            if (c != 0 && (c->isResolutionTarget || ! mustBeFlagged))
                return c;
        }

        for (i = 0; i < numIns; ++i)
        {
            Constant* const c = findTermToAdjust (term->getInput (i), mustBeFlagged);
            if (c != 0)
                return c;
        }

        return 0;
    }

    static bool containsAnySymbols (const Term* const t)
    {
        if (t->getType() == Expression::symbolType)
            return true;

        for (int i = t->getNumInputs(); --i >= 0;)
            if (containsAnySymbols (t->getInput (i)))
                return true;

        return false;
    }

    //==============================================================================
    class SymbolCheckVisitor  : public Term::SymbolVisitor
    {
    public:
        SymbolCheckVisitor (const Symbol& symbol_) : wasFound (false), symbol (symbol_) {}
        void useSymbol (const Symbol& s)    { wasFound = wasFound || s == symbol; }

        bool wasFound;

    private:
        const Symbol& symbol;

        JUCE_DECLARE_NON_COPYABLE (SymbolCheckVisitor);
    };

    //==============================================================================
    class SymbolListVisitor  : public Term::SymbolVisitor
    {
    public:
        SymbolListVisitor (Array<Symbol>& list_) : list (list_) {}
        void useSymbol (const Symbol& s)    { list.addIfNotAlreadyThere (s); }

    private:
        Array<Symbol>& list;

        JUCE_DECLARE_NON_COPYABLE (SymbolListVisitor);
    };

    //==============================================================================
    class Parser
    {
    public:
        //==============================================================================
        Parser (const String& stringToParse, int& textIndex_)
            : textString (stringToParse), textIndex (textIndex_)
        {
            text = textString;
        }

        const TermPtr readUpToComma()
        {
            if (textString.isEmpty())
                return new Constant (0.0, false);

            const TermPtr e (readExpression());

            if (e == 0 || ((! readOperator (",")) && text [textIndex] != 0))
                throw ParseError ("Syntax error: \"" + textString.substring (textIndex) + "\"");

            return e;
        }

    private:
        const String textString;
        const juce_wchar* text;
        int& textIndex;

        //==============================================================================
        static inline bool isDecimalDigit (const juce_wchar c) throw()
        {
            return c >= '0' && c <= '9';
        }

        void skipWhitespace (int& i) throw()
        {
            while (CharacterFunctions::isWhitespace (text [i]))
                ++i;
        }

        bool readChar (const juce_wchar required) throw()
        {
            if (text[textIndex] == required)
            {
                ++textIndex;
                return true;
            }

            return false;
        }

        bool readOperator (const char* ops, char* const opType = 0) throw()
        {
            skipWhitespace (textIndex);

            while (*ops != 0)
            {
                if (readChar (*ops))
                {
                    if (opType != 0)
                        *opType = *ops;

                    return true;
                }

                ++ops;
            }

            return false;
        }

        bool readIdentifier (String& identifier) throw()
        {
            skipWhitespace (textIndex);
            int i = textIndex;

            if (CharacterFunctions::isLetter (text[i]) || text[i] == '_')
            {
                ++i;

                while (CharacterFunctions::isLetterOrDigit (text[i]) || text[i] == '_')
                    ++i;
            }

            if (i > textIndex)
            {
                identifier = String (text + textIndex, i - textIndex);
                textIndex = i;
                return true;
            }

            return false;
        }

        Term* readNumber() throw()
        {
            skipWhitespace (textIndex);
            int i = textIndex;

            const bool isResolutionTarget = (text[i] == '@');
            if (isResolutionTarget)
            {
                ++i;
                skipWhitespace (i);
                textIndex = i;
            }

            if (text[i] == '-')
            {
                ++i;
                skipWhitespace (i);
            }

            int numDigits = 0;

            while (isDecimalDigit (text[i]))
            {
                ++i;
                ++numDigits;
            }

            const bool hasPoint = (text[i] == '.');

            if (hasPoint)
            {
                ++i;

                while (isDecimalDigit (text[i]))
                {
                    ++i;
                    ++numDigits;
                }
            }

            if (numDigits == 0)
                return 0;

            juce_wchar c = text[i];
            const bool hasExponent = (c == 'e' || c == 'E');

            if (hasExponent)
            {
                ++i;
                c = text[i];
                if (c == '+' || c == '-')
                    ++i;

                int numExpDigits = 0;
                while (isDecimalDigit (text[i]))
                {
                    ++i;
                    ++numExpDigits;
                }

                if (numExpDigits == 0)
                    return 0;
            }

            const int start = textIndex;
            textIndex = i;
            return new Constant (String (text + start, i - start).getDoubleValue(), isResolutionTarget);
        }

        const TermPtr readExpression()
        {
            TermPtr lhs (readMultiplyOrDivideExpression());

            char opType;
            while (lhs != 0 && readOperator ("+-", &opType))
            {
                TermPtr rhs (readMultiplyOrDivideExpression());

                if (rhs == 0)
                    throw ParseError ("Expected expression after \"" + String::charToString (opType) + "\"");

                if (opType == '+')
                    lhs = new Add (lhs, rhs);
                else
                    lhs = new Subtract (lhs, rhs);
            }

            return lhs;
        }

        const TermPtr readMultiplyOrDivideExpression()
        {
            TermPtr lhs (readUnaryExpression());

            char opType;
            while (lhs != 0 && readOperator ("*/", &opType))
            {
                TermPtr rhs (readUnaryExpression());

                if (rhs == 0)
                    throw ParseError ("Expected expression after \"" + String::charToString (opType) + "\"");

                if (opType == '*')
                    lhs = new Multiply (lhs, rhs);
                else
                    lhs = new Divide (lhs, rhs);
            }

            return lhs;
        }

        const TermPtr readUnaryExpression()
        {
            char opType;
            if (readOperator ("+-", &opType))
            {
                TermPtr term (readUnaryExpression());

                if (term == 0)
                    throw ParseError ("Expected expression after \"" + String::charToString (opType) + "\"");

                if (opType == '-')
                    term = term->negated();

                return term;
            }

            return readPrimaryExpression();
        }

        const TermPtr readPrimaryExpression()
        {
            TermPtr e (readParenthesisedExpression());
            if (e != 0)
                return e;

            e = readNumber();
            if (e != 0)
                return e;

            return readSymbolOrFunction();
        }

        const TermPtr readSymbolOrFunction()
        {
            String identifier;
            if (readIdentifier (identifier))
            {
                if (readOperator ("(")) // method call...
                {
                    Function* const f = new Function (identifier);
                    ScopedPointer<Term> func (f);  // (can't use ScopedPointer<Function> in MSVC)

                    TermPtr param (readExpression());

                    if (param == 0)
                    {
                        if (readOperator (")"))
                            return func.release();

                        throw ParseError ("Expected parameters after \"" + identifier + " (\"");
                    }

                    f->parameters.add (Expression (param));

                    while (readOperator (","))
                    {
                        param = readExpression();

                        if (param == 0)
                            throw ParseError ("Expected expression after \",\"");

                        f->parameters.add (Expression (param));
                    }

                    if (readOperator (")"))
                        return func.release();

                    throw ParseError ("Expected \")\"");
                }
                else if (readOperator ("."))
                {
                    TermPtr rhs (readSymbolOrFunction());

                    if (rhs == 0)
                        throw ParseError ("Expected symbol or function after \".\"");

                    if (identifier == "this")
                        return rhs;

                    return new DotOperator (new SymbolTerm (identifier), rhs);
                }
                else // just a symbol..
                {
                    jassert (identifier.trim() == identifier);
                    return new SymbolTerm (identifier);
                }
            }

            return 0;
        }

        const TermPtr readParenthesisedExpression()
        {
            if (! readOperator ("("))
                return 0;

            const TermPtr e (readExpression());
            if (e == 0 || ! readOperator (")"))
                return 0;

            return e;
        }

        JUCE_DECLARE_NON_COPYABLE (Parser);
    };
};

//==============================================================================
Expression::Expression()
    : term (new Expression::Helpers::Constant (0, false))
{
}

Expression::~Expression()
{
}

Expression::Expression (Term* const term_)
    : term (term_)
{
    jassert (term != 0);
}

Expression::Expression (const double constant)
    : term (new Expression::Helpers::Constant (constant, false))
{
}

Expression::Expression (const Expression& other)
    : term (other.term)
{
}

Expression& Expression::operator= (const Expression& other)
{
    term = other.term;
    return *this;
}

Expression::Expression (const String& stringToParse)
{
    int i = 0;
    Helpers::Parser parser (stringToParse, i);
    term = parser.readUpToComma();
}

const Expression Expression::parse (const String& stringToParse, int& textIndexToStartFrom)
{
    Helpers::Parser parser (stringToParse, textIndexToStartFrom);
    return Expression (parser.readUpToComma());
}

double Expression::evaluate() const
{
    return evaluate (Expression::Scope());
}

double Expression::evaluate (const Expression::Scope& scope) const
{
    try
    {
        return term->resolve (scope, 0)->toDouble();
    }
    catch (Helpers::EvaluationError&)
    {}

    return 0;
}

double Expression::evaluate (const Scope& scope, String& evaluationError) const
{
    try
    {
        return term->resolve (scope, 0)->toDouble();
    }
    catch (Helpers::EvaluationError& e)
    {
        evaluationError = e.description;
    }

    return 0;
}

const Expression Expression::operator+ (const Expression& other) const  { return Expression (new Helpers::Add (term, other.term)); }
const Expression Expression::operator- (const Expression& other) const  { return Expression (new Helpers::Subtract (term, other.term)); }
const Expression Expression::operator* (const Expression& other) const  { return Expression (new Helpers::Multiply (term, other.term)); }
const Expression Expression::operator/ (const Expression& other) const  { return Expression (new Helpers::Divide (term, other.term)); }
const Expression Expression::operator-() const                          { return Expression (term->negated()); }
const Expression Expression::symbol (const String& symbol)              { return Expression (new Helpers::SymbolTerm (symbol)); }

const Expression Expression::function (const String& functionName, const Array<Expression>& parameters)
{
    return Expression (new Helpers::Function (functionName, parameters));
}

const Expression Expression::adjustedToGiveNewResult (const double targetValue, const Expression::Scope& scope) const
{
    ScopedPointer<Term> newTerm (term->clone());

    Helpers::Constant* termToAdjust = Helpers::findTermToAdjust (newTerm, true);

    if (termToAdjust == 0)
        termToAdjust = Helpers::findTermToAdjust (newTerm, false);

    if (termToAdjust == 0)
    {
        newTerm = new Helpers::Add (newTerm.release(), new Helpers::Constant (0, false));
        termToAdjust = Helpers::findTermToAdjust (newTerm, false);
    }

    jassert (termToAdjust != 0);

    const Term* const parent = Helpers::findDestinationFor (newTerm, termToAdjust);

    if (parent == 0)
    {
        termToAdjust->value = targetValue;
    }
    else
    {
        const Helpers::TermPtr reverseTerm (parent->createTermToEvaluateInput (scope, termToAdjust, targetValue, newTerm));

        if (reverseTerm == 0)
            return Expression (targetValue);

        termToAdjust->value = reverseTerm->resolve (scope, 0)->toDouble();
    }

    return Expression (newTerm.release());
}

const Expression Expression::withRenamedSymbol (const Expression::Symbol& oldSymbol, const String& newName, const Scope& scope) const
{
    jassert (newName.toLowerCase().containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789_"));

    if (oldSymbol.symbolName == newName)
        return *this;

    Expression e (term->clone());
    e.term->renameSymbol (oldSymbol, newName, scope, 0);
    return e;
}

bool Expression::referencesSymbol (const Expression::Symbol& symbol, const Scope& scope) const
{
    Helpers::SymbolCheckVisitor visitor (symbol);

    try
    {
        term->visitAllSymbols (visitor, scope, 0);
    }
    catch (Helpers::EvaluationError&)
    {}

    return visitor.wasFound;
}

void Expression::findReferencedSymbols (Array<Symbol>& results, const Scope& scope) const
{
    try
    {
        Helpers::SymbolListVisitor visitor (results);
        term->visitAllSymbols (visitor, scope, 0);
    }
    catch (Helpers::EvaluationError&)
    {}
}

const String Expression::toString() const                   { return term->toString(); }
bool Expression::usesAnySymbols() const                     { return Helpers::containsAnySymbols (term); }
Expression::Type Expression::getType() const throw()        { return term->getType(); }
const String Expression::getSymbolOrFunction() const        { return term->getName(); }
int Expression::getNumInputs() const                        { return term->getNumInputs(); }
const Expression Expression::getInput (int index) const     { return Expression (term->getInput (index)); }

//==============================================================================
const ReferenceCountedObjectPtr<Expression::Term> Expression::Term::negated()
{
    return new Helpers::Negate (this);
}

//==============================================================================
Expression::ParseError::ParseError (const String& message)
    : description (message)
{
    DBG ("Expression::ParseError: " + message);
}

//==============================================================================
Expression::Symbol::Symbol (const String& scopeUID_, const String& symbolName_)
    : scopeUID (scopeUID_), symbolName (symbolName_)
{
}

bool Expression::Symbol::operator== (const Symbol& other) const throw()
{
    return symbolName == other.symbolName && scopeUID == other.scopeUID;
}

bool Expression::Symbol::operator!= (const Symbol& other) const throw()
{
    return ! operator== (other);
}

//==============================================================================
Expression::Scope::Scope()  {}
Expression::Scope::~Scope() {}

const Expression Expression::Scope::getSymbolValue (const String& symbol) const
{
    throw Helpers::EvaluationError ("Unknown symbol: " + symbol);
}

double Expression::Scope::evaluateFunction (const String& functionName, const double* parameters, int numParams) const
{
    if (numParams > 0)
    {
        if (functionName == "min")
        {
            double v = parameters[0];
            for (int i = 1; i < numParams; ++i)
                v = jmin (v, parameters[i]);

            return v;
        }
        else if (functionName == "max")
        {
            double v = parameters[0];
            for (int i = 1; i < numParams; ++i)
                v = jmax (v, parameters[i]);

            return v;
        }
        else if (numParams == 1)
        {
            if      (functionName == "sin")     return sin (parameters[0]);
            else if (functionName == "cos")     return cos (parameters[0]);
            else if (functionName == "tan")     return tan (parameters[0]);
            else if (functionName == "abs")     return std::abs (parameters[0]);
        }
    }

    throw Helpers::EvaluationError ("Unknown function: \"" + functionName + "\"");
}

void Expression::Scope::visitRelativeScope (const String& scopeName, Visitor&) const
{
    throw Helpers::EvaluationError ("Unknown symbol: " + scopeName);
}

const String Expression::Scope::getScopeUID() const
{
    return String::empty;
}


END_JUCE_NAMESPACE