// Ghidra headless script: decompile every function to <out>/decomp.c and list the defined strings
// with the functions that reference them to <out>/strings.txt.
// analyzeHeadless <proj> <name> -import FREQCCD_.EXE -postScript DumpAll.java <out>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;

public class DumpAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "decomp.c")))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// (decompile failed)");
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "strings.txt")))) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                StringBuilder refs = new StringBuilder();
                for (Reference ref : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    refs.append(" ").append(f != null ? f.getName() : ref.getFromAddress().toString());
                }
                w.println(d.getAddress() + "\t" + d.getValue() + "\t<-" + refs);
            }
        }
    }
}
