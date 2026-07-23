// Exports the current program's analyzed function map as stable CSV.
// @category SyphonFilterPC

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportFunctions extends GhidraScript {
    private static String csv(String value) {
        return "\"" + value.replace("\"", "\"\"") + "\"";
    }

    private static String address(ghidra.program.model.address.Address value) {
        String text = value.toString();
        return text.contains("::") ? text : "0x" + text;
    }

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 1) {
            throw new IllegalArgumentException("Usage: ExportFunctions.java <output.csv>");
        }

        File output = new File(arguments[0]);
        File parent = output.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        int count = 0;
        try (PrintWriter writer = new PrintWriter(output, StandardCharsets.UTF_8)) {
            writer.println("address,name,size,call_references,parameters,source,thunk");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            for (Function function : functions) {
                monitor.checkCancelled();
                int references = 0;
                ReferenceIterator iterator = currentProgram.getReferenceManager()
                    .getReferencesTo(function.getEntryPoint());
                while (iterator.hasNext()) {
                    iterator.next();
                    ++references;
                }

                writer.printf(
                    "%s,%s,%d,%d,%d,%s,%s%n",
                    address(function.getEntryPoint()),
                    csv(function.getName(true)),
                    function.getBody().getNumAddresses(),
                    references,
                    function.getParameterCount(),
                    function.getSymbol().getSource(),
                    function.isThunk());
                ++count;
            }
        }
        println("Exported " + count + " functions to " + output.getAbsolutePath());
    }
}
