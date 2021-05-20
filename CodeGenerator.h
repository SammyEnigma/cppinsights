/******************************************************************************
 *
 * C++ Insights, copyright (C) by Andreas Fertig
 * Distributed under an MIT license. See LICENSE for details
 *
 ****************************************************************************/

#ifndef INSIGHTS_CODE_GENERATOR_H
#define INSIGHTS_CODE_GENERATOR_H

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/APInt.h"

#include "ClangCompat.h"
#include "InsightsStaticStrings.h"
#include "InsightsStrongTypes.h"
#include "OutputFormatHelper.h"
#include "StackList.h"
//-----------------------------------------------------------------------------

namespace clang::insights {

/// \brief More or less the heart of C++ Insights.
///
/// This is the place where nearly all of the transformations happen. This class knows the needed types and how to
/// generated code from them.
class CodeGenerator
{
protected:
    OutputFormatHelper& mOutputFormatHelper;

    enum class LambdaCallerType
    {
        VarDecl,
        CallExpr,
        OperatorCallExpr,
        MemberCallExpr,
        LambdaExpr,
        ReturnStmt,
        BinaryOperator,
        CXXMethodDecl,
    };

    class LambdaHelper : public StackListEntry<LambdaHelper>
    {
    public:
        LambdaHelper(const LambdaCallerType lambdaCallerType, OutputFormatHelper& outputFormatHelper)
        : mLambdaCallerType{lambdaCallerType}
        , mCurrentPos{outputFormatHelper.CurrentPos()}
        , mOutputFormatHelper{outputFormatHelper}
        , mLambdaOutputFormatHelper{}
        , mInits{}
        {
            mLambdaOutputFormatHelper.SetIndent(mOutputFormatHelper);
        }

        void finish()
        {
            if(!mLambdaOutputFormatHelper.empty()) {
                mOutputFormatHelper.InsertAt(mCurrentPos, mLambdaOutputFormatHelper.GetString());
            }
        }

        OutputFormatHelper& buffer() { return mLambdaOutputFormatHelper; }

        std::string& inits() { return mInits; }

        void insertInits(OutputFormatHelper& outputFormatHelper)
        {
            if(!mInits.empty()) {
                outputFormatHelper.Append(mInits);
                mInits.clear();
            }
        }

        LambdaCallerType callerType() const { return mLambdaCallerType; }

    private:
        const LambdaCallerType mLambdaCallerType;
        const size_t           mCurrentPos;
        OutputFormatHelper&    mOutputFormatHelper;
        OutputFormatHelper     mLambdaOutputFormatHelper;
        std::string            mInits;
    };
    //-----------------------------------------------------------------------------

    using LambdaStackType = StackList<class LambdaHelper>;

public:
    explicit CodeGenerator(OutputFormatHelper& _outputFormatHelper)
    : mOutputFormatHelper{_outputFormatHelper}
    , mLambdaStackThis{}
    , mLambdaStack{mLambdaStackThis}
    , mSkipVarDecl{SkipVarDecl::No}
    , mUseCommaInsteadOfSemi{UseCommaInsteadOfSemi::No}
    , mLambdaExpr{nullptr}
    {
    }

    explicit CodeGenerator(OutputFormatHelper& _outputFormatHelper, LambdaStackType& lambdaStack)
    : mOutputFormatHelper{_outputFormatHelper}
    , mLambdaStackThis{}
    , mLambdaStack{lambdaStack}
    , mSkipVarDecl{SkipVarDecl::No}
    , mUseCommaInsteadOfSemi{UseCommaInsteadOfSemi::No}
    , mLambdaExpr{nullptr}
    {
    }

    virtual ~CodeGenerator() = default;

#define IGNORED_DECL(type)                                                                                             \
    virtual void InsertArg(const type*) {}
#define IGNORED_STMT(type)                                                                                             \
    virtual void InsertArg(const type*) {}
#define SUPPORTED_DECL(type) virtual void InsertArg(const type* stmt);
#define SUPPORTED_STMT(type) virtual void InsertArg(const type* stmt);

#include "CodeGeneratorTypes.h"

    virtual void InsertArg(const Decl* stmt);
    virtual void InsertArg(const Stmt* stmt);

    template<typename T>
    auto InsertTemplateArgs(const T& t) -> decltype((void)t.template_arguments(), void())
    {
        if constexpr(std::is_same_v<DeclRefExpr, T>) {
            if(0 == t.getNumTemplateArgs()) {
                return;
            }
        }

        InsertTemplateArgs(t.template_arguments());
    }

    template<typename T>
    auto InsertTemplateArgs(const T& t) -> decltype((void)t.asArray(), void())
    {
        InsertTemplateArgs(t.asArray());
    }

    void InsertTemplateArgs(const ClassTemplateSpecializationDecl& clsTemplateSpe);
    void InsertTemplateArgs(const FunctionDecl& FD)
    {
        if(const auto* tmplArgs = FD.getTemplateSpecializationArgs()) {
            InsertTemplateArgs(*tmplArgs);
        }
    }

    STRONG_BOOL(SkipAccess);

    /// \brief Insert the code for a FunctionDecl.
    ///
    /// This inserts the code of a FunctionDecl (and everything which is derived from one). It takes care of
    /// CXXMethodDecl's access modifier as well as things like constexpr, noexcept, static and more.
    ///
    /// @param decl The FunctionDecl to process.
    /// @param skipAccess Show or hide access modifiers (public, private, protected). The default is to show them.
    /// @param cxxInheritedCtorDecl If not null, the type and name of this decl is used for the parameters.
    void InsertAccessModifierAndNameWithReturnType(const FunctionDecl&       decl,
                                                   const SkipAccess          skipAccess           = SkipAccess::No,
                                                   const CXXConstructorDecl* cxxInheritedCtorDecl = nullptr);

    /// Track whether we have at least one local static variable in this TU.
    /// If so we need to insert the <new> header for the placement-new.
    static bool NeedToInsertNewHeader() { return mHaveLocalStatic; }

    template<typename T>
    void InsertTemplateArgs(const ArrayRef<T>& array)
    {
        mOutputFormatHelper.Append('<');

        ForEachArg(array, [&](const auto& arg) { InsertTemplateArg(arg); });

        /* put as space between to closing brackets: >> -> > > */
        if(mOutputFormatHelper.GetString().back() == '>') {
            mOutputFormatHelper.Append(' ');
        }

        mOutputFormatHelper.Append('>');
    }

    void InsertAttributes(const Decl::attr_range&);
    void InsertAttribute(const Attr&);

protected:
    virtual bool InsertVarDecl() { return true; }
    virtual bool InsertComma() { return false; }
    virtual bool InsertSemi() { return true; }
    virtual bool InsertNamespace() const { return false; }

    /// \brief Show casts to xvalues independent from the show all casts option.
    ///
    /// This helps showing xvalue casts in structured bindings.
    virtual bool ShowXValueCasts() const { return false; }

    void HandleTemplateParameterPack(const ArrayRef<TemplateArgument>& args);
    void HandleCompoundStmt(const CompoundStmt* stmt);
    /// \brief Show what is behind a local static variable.
    ///
    /// [stmt.dcl] p4: Initialization of a block-scope variable with static storage duration is thread-safe since C++11.
    /// Regardless of that, as long as it is a non-trivally construct and destructable class the compiler adds code to
    /// track the initialization state. Reference:
    /// - www.opensource.apple.com/source/libcppabi/libcppabi-14/src/cxa_guard.cxx
    void HandleLocalStaticNonTrivialClass(const VarDecl* stmt);

    void
    FormatCast(const std::string castName, const QualType& CastDestType, const Expr* SubExpr, const CastKind& castKind);

    template<typename T, typename Lambda>
    void ForEachArg(const T& arguments, Lambda&& lambda)
    {
        OutputFormatHelper::ForEachArg(arguments, mOutputFormatHelper, lambda);
    }

    void InsertArgWithParensIfNeeded(const Stmt* stmt);
    void InsertSuffix(const QualType& type);
    void InsertTemplateArg(const TemplateArgument& arg);
    void InsertTemplateArg(const TemplateArgumentLoc& arg) { InsertTemplateArg(arg.getArgument()); }
    bool InsertLambdaStaticInvoker(const CXXMethodDecl* cxxMethodDecl);
    void InsertTemplateParameters(const TemplateParameterList& list);

    STRONG_BOOL(InsertInline);

    void InsertConceptConstraint(const llvm::SmallVectorImpl<const Expr*>& constraints,
                                 const InsertInline                        insertInline);
    void InsertConceptConstraint(const FunctionDecl* tmplDecl);
    void InsertConceptConstraint(const VarDecl* varDecl);
    void InsertConceptConstraint(const TemplateParameterList& tmplDecl);

    template<typename T>
    void InsertQualifierAndNameWithTemplateArgs(const DeclarationName& declName, const T* stmt)
    {
        InsertQualifierAndName(declName, stmt->getQualifier(), stmt->hasTemplateKeyword());

        if(stmt->getNumTemplateArgs()) {
            InsertTemplateArgs(*stmt);
        } else if(stmt->hasExplicitTemplateArgs()) {
            // we have empty templates arguments, but angle brackets provided by the user
            mOutputFormatHelper.Append("<>");
        }
    }

    void InsertQualifierAndName(const DeclarationName&     declName,
                                const NestedNameSpecifier* qualifier,
                                const bool                 hasTemplateKeyword);

    /// For a special case, when a LambdaExpr occurs in a Constructor from an
    /// in class initializer, there is a need for a more narrow scope for the \c LAMBDA_SCOPE_HELPER.
    void InsertCXXMethodHeader(const CXXMethodDecl* stmt, OutputFormatHelper& initOutputFormatHelper);

    void InsertTemplateGuardBegin(const FunctionDecl* stmt);
    void InsertTemplateGuardEnd(const FunctionDecl* stmt);

    /// \brief Insert \c template<> to introduce a template specialization.
    void InsertTemplateSpecializationHeader() { mOutputFormatHelper.AppendNewLine("template<>"); }

    void InsertNamespace(const NestedNameSpecifier* namespaceSpecifier);
    void ParseDeclContext(const DeclContext* Ctx);

    STRONG_BOOL(SkipBody);
    void InsertCXXMethodDecl(const CXXMethodDecl* stmt, SkipBody skipBody);

    /// \brief Generalized function to insert either a \c CXXConstructExpr or \c CXXUnresolvedConstructExpr
    template<typename T>
    void InsertConstructorExpr(const T* stmt);

    /// \brief Check whether or not this statement will add curlys or parentheses and add them only if required.
    void InsertCurlysIfRequired(const Stmt* stmt);

    STRONG_BOOL(AddNewLineAfter);

    void WrapInCompoundIfNeeded(const Stmt* stmt, const AddNewLineAfter addNewLineAfter);

    STRONG_BOOL(AddSpaceAtTheEnd);

    enum class BraceKind
    {
        Parens,
        Curlys
    };

    template<typename T>
    void WrapInParens(T&& lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    template<typename T>
    void
    WrapInParensIfNeeded(bool needsParens, T&& lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    template<typename T>
    void
    WrapInCurliesIfNeeded(bool needsParens, T&& lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    template<typename T>
    void WrapInCurlys(T&& lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    template<typename T>
    void WrapInParensOrCurlys(const BraceKind        curlys,
                              T&&                    lambda,
                              const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void UpdateCurrentPos() { mCurrentPos = mOutputFormatHelper.CurrentPos(); }

    static const char* GetKind(const UnaryExprOrTypeTraitExpr& uk);
    static const char* GetBuiltinTypeSuffix(const BuiltinType::Kind& kind);

    class LambdaScopeHandler
    {
    public:
        LambdaScopeHandler(LambdaStackType&       stack,
                           OutputFormatHelper&    outputFormatHelper,
                           const LambdaCallerType lambdaCallerType);

        ~LambdaScopeHandler();

    private:
        LambdaStackType& mStack;
        LambdaHelper     mHelper;

        OutputFormatHelper& GetBuffer(OutputFormatHelper& outputFormatHelper) const;
    };

    void               HandleLambdaExpr(const LambdaExpr* stmt, LambdaHelper& lambdaHelper);
    static std::string FillConstantArray(const ConstantArrayType* ct, const std::string& value, const uint64_t startAt);
    static std::string GetValueOfValueInit(const QualType& t);

    LambdaStackType  mLambdaStackThis;
    LambdaStackType& mLambdaStack;

    STRONG_BOOL(SkipVarDecl);
    STRONG_BOOL(UseCommaInsteadOfSemi);
    STRONG_BOOL(NoEmptyInitList);
    STRONG_BOOL(ShowConstantExprValue);

    ShowConstantExprValue mShowConstantExprValue{ShowConstantExprValue::No};
    SkipVarDecl           mSkipVarDecl;
    UseCommaInsteadOfSemi mUseCommaInsteadOfSemi;
    NoEmptyInitList       mNoEmptyInitList{
        NoEmptyInitList::No};  //!< At least in case if a requires-clause containing T{} we don't want to get T{{}}.
    const LambdaExpr*  mLambdaExpr;
    static inline bool mHaveLocalStatic;  //!< Track whether there was a thread-safe \c static in the code. This
                                          //!< requires adding the \c <new> header.
    static constexpr auto MAX_FILL_VALUES_FOR_ARRAYS{
        uint64_t{100}};  //!< This is the upper limit of elements which will be shown for an array when filled by \c
                         //!< FillConstantArray.
    llvm::Optional<size_t> mCurrentPos{};        //!< The position in mOutputFormatHelper where a potential
                                                 //!< std::initializer_list expansion must be inserted.
    llvm::Optional<size_t> mCurrentReturnPos{};  //!< The position in mOutputFormatHelper from a return where a
                                                 //!< potential std::initializer_list expansion must be inserted.
    llvm::Optional<size_t> mCurrentFieldPos{};   //!< The position in mOutputFormatHelper in a class where where a
                                                 //!< potential std::initializer_list expansion must be inserted.
    OutputFormatHelper* mOutputFormatHelperOutside{
        nullptr};  //!< Helper output buffer for std::initializer_list expansion.
};
//-----------------------------------------------------------------------------

class LambdaCodeGenerator final : public CodeGenerator
{
public:
    using CodeGenerator::CodeGenerator;

    using CodeGenerator::InsertArg;
    void InsertArg(const CXXThisExpr* stmt) override;

    bool mCapturedThisAsCopy;
};
//-----------------------------------------------------------------------------

/*
 * \brief Special case to generate the inits of e.g. a \c ForStmt.
 *
 * This class is a specialization to handle cases where we can have multiple init statements to the same variable and
 * hence need only one time the \c VarDecl. An example a for-loops:
\code
for(int x=2, y=3, z=4; i < x; ++i) {}
\endcode
 */
class MultiStmtDeclCodeGenerator final : public CodeGenerator
{
public:
    using CodeGenerator::CodeGenerator;

    // Insert the semi after the last declaration. This implies that this class always requires its own scope.
    ~MultiStmtDeclCodeGenerator() { mOutputFormatHelper.Append("; "); }

    using CodeGenerator::InsertArg;

protected:
    OnceTrue  mInsertVarDecl;  //! Insert the \c VarDecl only once.
    OnceFalse mInsertComma;    //! Insert the comma after we have generated the first \c VarDecl and we are about to
                               //! insert another one.

    bool InsertVarDecl() override { return mInsertVarDecl; }
    bool InsertComma() override { return mInsertComma; }
    bool InsertSemi() override { return false; }
};
//-----------------------------------------------------------------------------

}  // namespace clang::insights

#endif /* INSIGHTS_CODE_GENERATOR_H */
