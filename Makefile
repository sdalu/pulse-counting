
pulse-counting: src/pulse-counting.o
	$(CC) $(LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

clean:
	rm -f src/*.o pulse-counting
