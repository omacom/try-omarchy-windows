package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"strings"
	"testing"
)

// syscall.NewCallback permanently reserves one of a hard 2000-entry table and
// never frees it, so any call on a repeating code path eventually kills the
// process with "too many callback functions". enforceTitle runs once a second
// for the whole life of the app, so a callback built inside it killed the
// launcher after ~33 minutes and silently took the Windows-key hook with it.
// Parsed as source because winapi.go is a Windows-only file and CI runs Linux.
func TestEnforceTitleBuildsNoCallback(t *testing.T) {
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "winapi.go", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	var checked bool
	ast.Inspect(file, func(n ast.Node) bool {
		fn, ok := n.(*ast.FuncDecl)
		if !ok || fn.Name.Name != "enforceTitle" {
			return true
		}
		checked = true
		ast.Inspect(fn.Body, func(inner ast.Node) bool {
			sel, ok := inner.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "NewCallback" {
				return true
			}
			if pkg, ok := sel.X.(*ast.Ident); ok && pkg.Name == "syscall" {
				t.Errorf("enforceTitle builds a callback at %s: it runs once a second, "+
					"so this exhausts the 2000-entry table and kills the launcher",
					fset.Position(inner.Pos()))
			}
			return true
		})
		return false
	})
	if !checked {
		t.Fatal("enforceTitle not found in winapi.go - update this guard")
	}
}

// The callback must stay a package-level singleton for the fix to hold.
func TestEnumTitleCallbackIsPackageLevel(t *testing.T) {
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "winapi.go", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	for _, decl := range file.Decls {
		gen, ok := decl.(*ast.GenDecl)
		if !ok || gen.Tok != token.VAR {
			continue
		}
		for _, spec := range gen.Specs {
			vs, ok := spec.(*ast.ValueSpec)
			if !ok {
				continue
			}
			for i, name := range vs.Names {
				if name.Name != "enumTitleCallback" || i >= len(vs.Values) {
					continue
				}
				if strings.Contains(exprString(fset, vs.Values[i]), "NewCallback") {
					return
				}
			}
		}
	}
	t.Fatal("enumTitleCallback is not a package-level syscall.NewCallback var")
}

func exprString(fset *token.FileSet, e ast.Expr) string {
	var sb strings.Builder
	ast.Inspect(e, func(n ast.Node) bool {
		if id, ok := n.(*ast.Ident); ok {
			sb.WriteString(id.Name + " ")
		}
		return true
	})
	return sb.String()
}
