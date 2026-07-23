// Decompiles every analyzed function in one overlay address space.
// @category SyphonFilterPC

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportOverlayDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) {
            throw new IllegalArgumentException(
                "Usage: ExportOverlayDecomp.java <address-space> <output.c>");
        }

        String addressSpace = arguments[0];
        File output = new File(arguments[1]);
        File parent = output.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        DecompInterface decompiler = new DecompInterface();
        int count = 0;
        try {
            decompiler.toggleCCode(true);
            decompiler.toggleSyntaxTree(true);
            if (!decompiler.openProgram(currentProgram)) {
                throw new IllegalStateException(decompiler.getLastMessage());
            }

            try (PrintWriter writer = new PrintWriter(output, StandardCharsets.UTF_8)) {
                FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
                for (Function function : functions) {
                    monitor.checkCancelled();
                    if (!function.getEntryPoint().getAddressSpace().getName().equals(addressSpace)) {
                        continue;
                    }

                    DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                    if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                        throw new IllegalStateException(
                            "Cannot decompile " + function.getName(true) + ": " + result.getErrorMessage());
                    }
                    writer.println("/* " + function.getEntryPoint() + " " + function.getName(true) + " */");
                    writer.println(result.getDecompiledFunction().getC());
                    ++count;
                }
            }
        } finally {
            decompiler.dispose();
        }
        println("Decompiled " + count + " functions from " + addressSpace +
            " to " + output.getAbsolutePath());
    }
}
