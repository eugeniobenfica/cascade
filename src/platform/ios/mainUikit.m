/* iOS: the program's entry point. UIKit owns the main thread from UIApplicationMain on, so main is
 * cascade's: it starts UIKit, and once the application has launched it calls the program's main
 * (renamed cscdAppMain by window.h) on the main thread. The program's loop then runs UIKit's run
 * loop itself (cscdWindowPump), as on the other systems. Kept apart from windowUikit.m so that it
 * is linked in only when the program has no main of its own.
 * Objective-C with ARC (CMakeLists.txt). */
#include "platform/common/platformInternal.h"

#import <UIKit/UIKit.h>

#undef main
int cscdAppMain(int argc, char** argv); /* the program's main (window.h) */

static int programArgc;
static char** programArgv;

@interface CascadeAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation CascadeAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    /* after this returns: a timer, not the main queue, so that the queue keeps running while the
     * program's loop runs inside it */
    [self performSelector:@selector(runProgram) withObject:nil afterDelay:0];
    return YES;
}

- (void)runProgram {
    const int code = cscdAppMain(programArgc, programArgv);
    fflush(stdout);
    fflush(stderr);
    exit(code); /* main returned: the program is over */
}
@end

int main(int argc, char* argv[]) {
    programArgc = argc;
    programArgv = argv;
    uikitStarted = true;
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(CascadeAppDelegate.class));
    }
}
