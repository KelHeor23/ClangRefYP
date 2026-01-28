#include "RefactorTool.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include <gtest/gtest.h>
#include <string>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

std::string applyRefactorings(const std::string &Code) {
    std::string ResultCode;

    class TestAction : public clang::ASTFrontendAction {
    public:
        TestAction(std::string &OutResult) : OutResult(OutResult) {}

        std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
            TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
            return std::make_unique<ComplexConsumer>(TheRewriter);
        }

        void EndSourceFileAction() override {
            const clang::SourceManager &SM = TheRewriter.getSourceMgr();
            clang::FileID ID = SM.getMainFileID();
            if (const llvm::RewriteBuffer *Buf = TheRewriter.getRewriteBufferFor(ID)) {
                OutResult = std::string(Buf->begin(), Buf->end());
            }
        }

    private:
        clang::Rewriter TheRewriter;
        std::string &OutResult;
    };

    std::vector<std::string> Args = {"-std=c++17", "-fno-delayed-template-parsing"};
    bool Success =
        clang::tooling::runToolOnCodeWithArgs(std::make_unique<TestAction>(ResultCode), Code, Args, "input.cc");

    return Success && !ResultCode.empty() ? ResultCode : Code;
}

TEST(VirtualDtorTest, AddsVirtualWhenDerivedExists) {
    constexpr const char *Input = R"cpp(
class Base { 
public:
    ~Base() {} 
};
class Derived : public Base {};
)cpp";

    constexpr const char *Expected = R"cpp(
class Base { 
public:
    virtual ~Base() {} 
};
class Derived : public Base {};
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}

TEST(VirtualDtorTest, DoesNotAddVirtualToStandaloneClass) {
    constexpr const char *Input = R"cpp(
class A { ~A() {} };
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Input);
}

TEST(OverrideTest, AddsOverrideToOverridingMethod) {
    constexpr const char *Input = R"cpp(
class B { virtual void f(); };
class D : public B { void f() {} };
)cpp";

    constexpr const char *Expected = R"cpp(
class B { virtual void f(); };
class D : public B { void f() override {} };
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}

TEST(OverrideTest, DoesNotAddOverrideToNonVirtualMethod) {
    constexpr const char *Input = R"cpp(
class X { void g() {} };
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Input);
}

TEST(OverrideTest, AddsOverrideToPureVirtualMethodImplementation) {
    constexpr const char *Input = R"cpp(
class Base {
public:
    virtual void foo() = 0;
};

class Derived : public Base {
public:
    void foo() override {}
};
)cpp";

    // Этот класс не должен измениться
    EXPECT_EQ(applyRefactorings(Input), Input);
}

TEST(OverrideTest, AddsOverrideBeforePureSpecifierInDeclaration) {
    constexpr const char *Input = R"cpp(
class Base {
public:
    virtual void foo() = 0;
};

class Derived : public Base {
public:
    void foo() = 0;
};
)cpp";

    constexpr const char *Expected = R"cpp(
class Base {
public:
    virtual void foo() = 0;
};

class Derived : public Base {
public:
    void foo() override = 0;
};
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}

TEST(RangeForTest, AddsAmpersandToConstStdString) {
    constexpr const char *Input = R"cpp(
#include <string>
#include <vector>
void foo() {
    std::vector<std::string> v;
    for (const std::string s : v) {}
}
)cpp";

    constexpr const char *Expected = R"cpp(
#include <string>
#include <vector>
void foo() {
    std::vector<std::string> v;
    for (const std::string& s : v) {}
}
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}

TEST(RangeForTest, DoesNotAddAmpersandToInt) {
    constexpr const char *Input = R"cpp(
#include <vector>
void foo() {
    std::vector<int> v;
    for (const int x : v) {}
}
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Input);
}

TEST(RefactoringTest, MultipleChangesInOneFile) {
    constexpr const char *Input = R"cpp(
class Base {
public:
    ~Base() {}
    virtual void foo() {}
};

class Derived : public Base {
public:
    void foo() {}
};

#include <vector>
#include <string>
void test() {
    std::vector<std::string> vec;
    for (const std::string item : vec) {}
}
)cpp";

    constexpr const char *Expected = R"cpp(
class Base {
public:
    virtual ~Base() {}
    virtual void foo() {}
};

class Derived : public Base {
public:
    void foo() override {}
};

#include <vector>
#include <string>
void test() {
    std::vector<std::string> vec;
    for (const std::string& item : vec) {}
}
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}

TEST(RefactoringTest, TemplateClassWithDestructor) {
    constexpr const char *Input = R"cpp(
template<typename T>
class Base {
public:
    ~Base() {}
};

class Derived : public Base<int> {};
)cpp";

    constexpr const char *Expected = R"cpp(
template<typename T>
class Base {
public:
    virtual ~Base() {}
};

class Derived : public Base<int> {};
)cpp";

    EXPECT_EQ(applyRefactorings(Input), Expected);
}
