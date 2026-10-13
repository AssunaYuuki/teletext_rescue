// Ghidra headless script: write the whole listing (labels, instructions, data) to <out>/listing.asm.
// analyzeHeadless <proj> <name> -process FREQCCD_.EXE -noanalysis -postScript DumpAsm.java <out>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DumpAsm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        Listing l = currentProgram.getListing();
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "listing.asm")))) {
            CodeUnitIterator it = l.getCodeUnits(true);
            while (it.hasNext()) {
                CodeUnit cu = it.next();
                Function f = getFunctionAt(cu.getAddress());
                if (f != null) w.println("\n;=========== " + f.getName());
                Symbol s = getSymbolAt(cu.getAddress());
                if (s != null && f == null) w.println(s.getName() + ":");
                if (cu instanceof Data && !((Data) cu).isDefined()) continue;
                StringBuilder refs = new StringBuilder();
                for (Reference r : cu.getReferencesFrom()) {
                    Data d = getDataAt(r.getToAddress());
                    if (d != null && d.hasStringValue()) refs.append(" \"").append(d.getValue()).append("\"");
                    else { Function g = getFunctionAt(r.getToAddress()); if (g != null) refs.append(" ->").append(g.getName()); }
                }
                w.println(cu.getAddress() + "  " + cu.toString() + (refs.length() > 0 ? "   ;" + refs : ""));
            }
        }
    }
}
