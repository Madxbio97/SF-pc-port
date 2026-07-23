// Exports references to an address with containing function names.
// @category SyphonFilterPC

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportReferences extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) {
            throw new IllegalArgumentException("Usage: ExportReferences.java <address> <output.txt>");
        }

        Address target = toAddr(arguments[0]);
        File output = new File(arguments[1]);
        File parent = output.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        int count = 0;
        try (PrintWriter writer = new PrintWriter(output, StandardCharsets.UTF_8)) {
            writer.println("target=" + target);
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(target);
            while (references.hasNext()) {
                monitor.checkCancelled();
                Reference reference = references.next();
                Function function = currentProgram.getFunctionManager()
                    .getFunctionContaining(reference.getFromAddress());
                writer.println(
                    reference.getFromAddress() + " " + reference.getReferenceType() + " " +
                    (function == null ? "<no function>" : function.getName(true)));
                ++count;
            }
        }
        println("Exported " + count + " references to " + output.getAbsolutePath());
    }
}
