// Decompiles the function at a requested address into a text file.
// @category SyphonFilterPC

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) {
            throw new IllegalArgumentException("Usage: ExportDecomp.java <address> <output.c>");
        }

        Address address = toAddr(arguments[0]);
        Function function = currentProgram.getFunctionManager().getFunctionAt(address);
        if (function == null) {
            function = currentProgram.getFunctionManager().getFunctionContaining(address);
        }
        if (function == null) {
            throw new IllegalArgumentException("No function at " + address);
        }

        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.toggleCCode(true);
            decompiler.toggleSyntaxTree(true);
            if (!decompiler.openProgram(currentProgram)) {
                throw new IllegalStateException(decompiler.getLastMessage());
            }
            DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
            if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                throw new IllegalStateException(result.getErrorMessage());
            }

            File output = new File(arguments[1]);
            File parent = output.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            Files.writeString(
                output.toPath(),
                result.getDecompiledFunction().getC(),
                StandardCharsets.UTF_8);
            println("Decompiled " + function.getName(true) + " to " + output.getAbsolutePath());
        } finally {
            decompiler.dispose();
        }
    }
}
