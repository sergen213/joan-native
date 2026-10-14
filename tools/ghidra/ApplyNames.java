// Apply analyst function names from a TSV (addr<TAB>name[<TAB>anything]) to joan.exe.
// Lines starting with '#' and blank lines are ignored. Other programs are left untouched.
// Creates a function at addr if none exists. Usage: ApplyNames.java <names.tsv>
//@category JoA

import java.io.File;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
	@Override
	public void run() throws Exception {
		if (!currentProgram.getName().equalsIgnoreCase("joan.exe")) return;
		File f = new File(getScriptArgs()[0]);
		if (!f.exists()) return;
		int n = 0;
		for (String line : Files.readAllLines(f.toPath())) {
			if (line.isBlank() || line.startsWith("#")) continue;
			String[] c = line.split("\t");
			if (c.length < 2) continue;
			Address a = toAddr(c[0].trim());
			Function fn = getFunctionAt(a);
			if (fn == null) {
				disassemble(a);
				fn = createFunction(a, null);
			}
			if (fn == null) {
				printerr("ApplyNames: no function at " + a);
				continue;
			}
			fn.setName(c[1].trim(), SourceType.USER_DEFINED);
			n++;
		}
		println("ApplyNames: applied " + n);
	}
}
