
all:
	tools/dev/gm.py x64.debug

test:
	out/x64.debug/d8 test.js

clean:
	echo "do nothing"
