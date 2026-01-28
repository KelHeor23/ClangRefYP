#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем. 
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto& Diag = Result.Context->getDiagnostics();
    auto& SM = *Result.SourceManager; // Получаем SourceManager для проверки isInMainFile
    
    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor,
                            DiagnosticsEngine &Diag,
                            SourceManager &SM) {
    if (!SM.isInMainFile(Dtor->getLocation()))
        return;

    const CXXRecordDecl *Base = Dtor->getParent();
    if (!Base || !Base->getDefinition())
        return;

    bool hasDerived = false;
    for (auto it = Base->redecls_begin(); it != Base->redecls_end(); ++it) {
        if (auto *CRD = dyn_cast<CXXRecordDecl>(*it)) {
            for (auto *OtherDecl : CRD->getDeclContext()->decls()) {
                if (auto *OtherCRD = dyn_cast<CXXRecordDecl>(OtherDecl)) {
                    if (OtherCRD->isDerivedFrom(CRD) && OtherCRD != CRD) {
                        hasDerived = true;
                        break;
                    }
                }
            }
            if (hasDerived) break;
        }
    }
    
    if (!hasDerived)
        return;

    unsigned rawLoc = SM.getSpellingLoc(Dtor->getLocation()).getRawEncoding();
    if (!virtualDtorLocations.emplace(rawLoc).second)
        return;

    SourceLocation dtorStart = Dtor->getSourceRange().getBegin();
    if (dtorStart.isValid()) {
        Rewrite.InsertTextBefore(dtorStart, "virtual ");
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark,
                                                 "Added 'virtual' to destructor of base class with derived types");
    Diag.Report(Dtor->getLocation(), DiagID);
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method,
                            DiagnosticsEngine &Diag,
                            SourceManager &SM) {
    if (!SM.isInMainFile(Method->getLocation()))
        return;

    unsigned rawLoc = SM.getSpellingLoc(Method->getLocation()).getRawEncoding();
    if (!overrideLocations.emplace(rawLoc).second)
        return;

    SourceLocation InsertLoc;

    if (const auto *TSI = Method->getTypeSourceInfo()) {
        if (auto FTL = TSI->getTypeLoc().getAs<FunctionTypeLoc>()) {
            InsertLoc = Lexer::getLocForEndOfToken(FTL.getRParenLoc(), 0, SM, Rewrite.getLangOpts());
        }
    }

    if (InsertLoc.isInvalid())
        return;

    Rewrite.InsertText(InsertLoc, " override", false, false);

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Added 'override' to overriding method");
    Diag.Report(Method->getLocation(), DiagID);
}

//todo: необходимо реализовать обработку случая отсутствие & в range-for
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar,
                                        DiagnosticsEngine &Diag,
                                        SourceManager &SM){
    if (!SM.isInMainFile(LoopVar->getLocation()))
        return;

    unsigned rawLoc = SM.getSpellingLoc(LoopVar->getLocation()).getRawEncoding();
    if (!crangeForLocations.emplace(rawLoc).second)
        return;

    TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI)
        return;

    TypeLoc TL = TSI->getTypeLoc();
    SourceLocation EndLoc = TL.getEndLoc();
    if (EndLoc.isInvalid())
        return;

    Rewrite.InsertTextAfterToken(EndLoc, "&");

    const unsigned DiagID =
        Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Added '&' to const loop variable to avoid copying");
    Diag.Report(LoopVar->getLocation(), DiagID);
}

auto NvDtorMatcher()
{
    // Ищем невиртуальные деструкторы в классах, которые имеют производные классы
    return cxxDestructorDecl(
        unless(isVirtual()), 
        unless(isImplicit()), 
        isExpansionInMainFile()
    ).bind("nonVirtualDtor");
}

auto NoOverrideMatcher()
{
    // Ищем методы, которые переопределяют базовые методы, но не имеют атрибута override
    return cxxMethodDecl(
        isOverride(),
        unless(hasAttr(clang::attr::Override)),
        unless(isImplicit()),
        unless(cxxDestructorDecl()),
        isExpansionInMainFile()
    ).bind("missingOverride");
}

auto NoRefConstVarInRangeLoopMatcher()
{
    //todo: замените код ниже, на свою реализацию, необходимо реализовать матчеры для поиска range-for без &
    return cxxForRangeStmt(
        hasLoopVariable(
            varDecl(
                hasType(qualType(
                    isConstQualified(),
                    unless(referenceType()),
                    unless(hasCanonicalType(builtinType()))
                )),
                isExpansionInMainFile()
            ).bind("loopVar")
        )
    );
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
    Finder.matchAST(Context);
}


std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI,
                                                StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(
        RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction( CompilerInstance &CI) {
// Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(),
                                        CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}

/*
int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}
*/