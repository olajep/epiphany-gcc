// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Package testing provides support for automated testing of Go packages.
// It is intended to be used in concert with the ``gotest'' utility, which automates
// execution of any function of the form
//     func TestXxx(*testing.T)
// where Xxx can be any alphanumeric string (but the first letter must not be in
// [a-z]) and serves to identify the test routine.
// These TestXxx routines should be declared within the package they are testing.
//
// Functions of the form
//     func BenchmarkXxx(*testing.B)
// are considered benchmarks, and are executed by gotest when the -test.bench
// flag is provided.
//
// A sample benchmark function looks like this:
//     func BenchmarkHello(b *testing.B) {
//         for i := 0; i < b.N; i++ {
//             fmt.Sprintf("hello")
//         }
//     }
// The benchmark package will vary b.N until the benchmark function lasts
// long enough to be timed reliably.  The output
//     testing.BenchmarkHello    10000000    282 ns/op
// means that the loop ran 10000000 times at a speed of 282 ns per loop.
//
// If a benchmark needs some expensive setup before running, the timer
// may be stopped:
//     func BenchmarkBigLen(b *testing.B) {
//         b.StopTimer()
//         big := NewBig()
//         b.StartTimer()
//         for i := 0; i < b.N; i++ {
//             big.Len()
//         }
//     }
package testing

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/pprof"
	"strconv"
	"strings"
	"time"
)

var (
	// The short flag requests that tests run more quickly, but its functionality
	// is provided by test writers themselves.  The testing package is just its
	// home.  The all.bash installation script sets it to make installation more
	// efficient, but by default the flag is off so a plain "gotest" will do a
	// full test of the package.
	short = flag.Bool("test.short", false, "run smaller test suite to save time")

	// Report as tests are run; default is silent for success.
	chatty         = flag.Bool("test.v", false, "verbose: print additional output")
	match          = flag.String("test.run", "", "regular expression to select tests to run")
	memProfile     = flag.String("test.memprofile", "", "write a memory profile to the named file after execution")
	memProfileRate = flag.Int("test.memprofilerate", 0, "if >=0, sets runtime.MemProfileRate")
	cpuProfile     = flag.String("test.cpuprofile", "", "write a cpu profile to the named file during execution")
	timeout        = flag.Int64("test.timeout", 0, "if > 0, sets time limit for tests in seconds")
	cpuListStr     = flag.String("test.cpu", "", "comma-separated list of number of CPUs to use for each test")
	parallel       = flag.Int("test.parallel", runtime.GOMAXPROCS(0), "maximum test parallelism")

	cpuList []int
)

// common holds the elements common between T and B and
// captures common methods such as Errorf.
type common struct {
	output   []byte    // Output generated by test or benchmark.
	failed   bool      // Test or benchmark has failed.
	start    time.Time // Time test or benchmark started
	duration time.Duration
	self     interface{}      // To be sent on signal channel when done.
	signal   chan interface{} // Output for serial tests.
}

// Short reports whether the -test.short flag is set.
func Short() bool {
	return *short
}

// decorate inserts the final newline if needed and indentation tabs for formatting.
// If addFileLine is true, it also prefixes the string with the file and line of the call site.
func decorate(s string, addFileLine bool) string {
	if addFileLine {
		_, file, line, ok := runtime.Caller(4) // decorate + log + public function.
		if ok {
			// Truncate file name at last file name separator.
			if index := strings.LastIndex(file, "/"); index >= 0 {
				file = file[index+1:]
			} else if index = strings.LastIndex(file, "\\"); index >= 0 {
				file = file[index+1:]
			}
		} else {
			file = "???"
			line = 1
		}
		s = fmt.Sprintf("%s:%d: %s", file, line, s)
	}
	s = "\t" + s // Every line is indented at least one tab.
	n := len(s)
	if n > 0 && s[n-1] != '\n' {
		s += "\n"
		n++
	}
	for i := 0; i < n-1; i++ { // -1 to avoid final newline
		if s[i] == '\n' {
			// Second and subsequent lines are indented an extra tab.
			return s[0:i+1] + "\t" + decorate(s[i+1:n], false)
		}
	}
	return s
}

// T is a type passed to Test functions to manage test state and support formatted test logs.
// Logs are accumulated during execution and dumped to standard error when done.
type T struct {
	common
	name          string    // Name of test.
	startParallel chan bool // Parallel tests will wait on this.
}

// Fail marks the function as having failed but continues execution.
func (c *common) Fail() { c.failed = true }

// Failed returns whether the function has failed.
func (c *common) Failed() bool { return c.failed }

// FailNow marks the function as having failed and stops its execution.
// Execution will continue at the next Test.
func (c *common) FailNow() {
	c.duration = time.Now().Sub(c.start)
	c.Fail()
	c.signal <- c.self
	runtime.Goexit()
}

// log generates the output. It's always at the same stack depth.
func (c *common) log(s string) {
	c.output = append(c.output, decorate(s, true)...)
}

// Log formats its arguments using default formatting, analogous to Println(),
// and records the text in the error log.
func (c *common) Log(args ...interface{}) { c.log(fmt.Sprintln(args...)) }

// Logf formats its arguments according to the format, analogous to Printf(),
// and records the text in the error log.
func (c *common) Logf(format string, args ...interface{}) { c.log(fmt.Sprintf(format, args...)) }

// Error is equivalent to Log() followed by Fail().
func (c *common) Error(args ...interface{}) {
	c.log(fmt.Sprintln(args...))
	c.Fail()
}

// Errorf is equivalent to Logf() followed by Fail().
func (c *common) Errorf(format string, args ...interface{}) {
	c.log(fmt.Sprintf(format, args...))
	c.Fail()
}

// Fatal is equivalent to Log() followed by FailNow().
func (c *common) Fatal(args ...interface{}) {
	c.log(fmt.Sprintln(args...))
	c.FailNow()
}

// Fatalf is equivalent to Logf() followed by FailNow().
func (c *common) Fatalf(format string, args ...interface{}) {
	c.log(fmt.Sprintf(format, args...))
	c.FailNow()
}

// Parallel signals that this test is to be run in parallel with (and only with) 
// other parallel tests in this CPU group.
func (t *T) Parallel() {
	t.signal <- (*T)(nil) // Release main testing loop
	<-t.startParallel     // Wait for serial tests to finish
}

// An internal type but exported because it is cross-package; part of the implementation
// of gotest.
type InternalTest struct {
	Name string
	F    func(*T)
}

func tRunner(t *T, test *InternalTest) {
	t.start = time.Now()
	test.F(t)
	t.duration = time.Now().Sub(t.start)
	t.signal <- t
}

// An internal function but exported because it is cross-package; part of the implementation
// of gotest.
func Main(matchString func(pat, str string) (bool, error), tests []InternalTest, benchmarks []InternalBenchmark, examples []InternalExample) {
	flag.Parse()
	parseCpuList()

	before()
	startAlarm()
	testOk := RunTests(matchString, tests)
	exampleOk := RunExamples(examples)
	if !testOk || !exampleOk {
		fmt.Println("FAIL")
		os.Exit(1)
	}
	fmt.Println("PASS")
	stopAlarm()
	RunBenchmarks(matchString, benchmarks)
	after()
}

func (t *T) report() {
	tstr := fmt.Sprintf("(%.2f seconds)", t.duration.Seconds())
	format := "--- %s: %s %s\n%s"
	if t.failed {
		fmt.Printf(format, "FAIL", t.name, tstr, t.output)
	} else if *chatty {
		fmt.Printf(format, "PASS", t.name, tstr, t.output)
	}
}

func RunTests(matchString func(pat, str string) (bool, error), tests []InternalTest) (ok bool) {
	ok = true
	if len(tests) == 0 {
		fmt.Fprintln(os.Stderr, "testing: warning: no tests to run")
		return
	}
	for _, procs := range cpuList {
		runtime.GOMAXPROCS(procs)
		// We build a new channel tree for each run of the loop.
		// collector merges in one channel all the upstream signals from parallel tests.
		// If all tests pump to the same channel, a bug can occur where a test
		// kicks off a goroutine that Fails, yet the test still delivers a completion signal,
		// which skews the counting.
		var collector = make(chan interface{})

		numParallel := 0
		startParallel := make(chan bool)

		for i := 0; i < len(tests); i++ {
			matched, err := matchString(*match, tests[i].Name)
			if err != nil {
				fmt.Fprintf(os.Stderr, "testing: invalid regexp for -test.run: %s\n", err)
				os.Exit(1)
			}
			if !matched {
				continue
			}
			testName := tests[i].Name
			if procs != 1 {
				testName = fmt.Sprintf("%s-%d", tests[i].Name, procs)
			}
			t := &T{
				common: common{
					signal: make(chan interface{}),
				},
				name:          testName,
				startParallel: startParallel,
			}
			t.self = t
			if *chatty {
				fmt.Printf("=== RUN %s\n", t.name)
			}
			go tRunner(t, &tests[i])
			out := (<-t.signal).(*T)
			if out == nil { // Parallel run.
				go func() {
					collector <- <-t.signal
				}()
				numParallel++
				continue
			}
			t.report()
			ok = ok && !out.failed
		}

		running := 0
		for numParallel+running > 0 {
			if running < *parallel && numParallel > 0 {
				startParallel <- true
				running++
				numParallel--
				continue
			}
			t := (<-collector).(*T)
			t.report()
			ok = ok && !t.failed
			running--
		}
	}
	return
}

// before runs before all testing.
func before() {
	if *memProfileRate > 0 {
		runtime.MemProfileRate = *memProfileRate
	}
	if *cpuProfile != "" {
		f, err := os.Create(*cpuProfile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "testing: %s", err)
			return
		}
		if err := pprof.StartCPUProfile(f); err != nil {
			fmt.Fprintf(os.Stderr, "testing: can't start cpu profile: %s", err)
			f.Close()
			return
		}
		// Could save f so after can call f.Close; not worth the effort.
	}

}

// after runs after all testing.
func after() {
	if *cpuProfile != "" {
		pprof.StopCPUProfile() // flushes profile to disk
	}
	if *memProfile != "" {
		f, err := os.Create(*memProfile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "testing: %s", err)
			return
		}
		if err = pprof.WriteHeapProfile(f); err != nil {
			fmt.Fprintf(os.Stderr, "testing: can't write %s: %s", *memProfile, err)
		}
		f.Close()
	}
}

var timer *time.Timer

// startAlarm starts an alarm if requested.
func startAlarm() {
	if *timeout > 0 {
		timer = time.AfterFunc(time.Duration(*timeout)*time.Second, alarm)
	}
}

// stopAlarm turns off the alarm.
func stopAlarm() {
	if *timeout > 0 {
		timer.Stop()
	}
}

// alarm is called if the timeout expires.
func alarm() {
	panic("test timed out")
}

func parseCpuList() {
	if len(*cpuListStr) == 0 {
		cpuList = append(cpuList, runtime.GOMAXPROCS(-1))
	} else {
		for _, val := range strings.Split(*cpuListStr, ",") {
			cpu, err := strconv.Atoi(val)
			if err != nil || cpu <= 0 {
				fmt.Fprintf(os.Stderr, "testing: invalid value %q for -test.cpu", val)
				os.Exit(1)
			}
			cpuList = append(cpuList, cpu)
		}
	}
}
