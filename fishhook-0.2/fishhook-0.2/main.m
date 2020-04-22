//
//  main.m
//  fishhook-0.2
//
//  Created by Leo on 2020/4/22.
//  Copyright © 2020 LEO. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "fishhook.h"

static int (*original_strlen)(const char *_s);

int new_strlen(const char *s) {
    return 100;
}

int main(int argc, const char * argv[]) {
    
    struct rebinding strlen_rebinding = { "strlen", new_strlen, (void *)&original_strlen};
    rebind_symbols((struct rebinding[1]){ strlen_rebinding}, 1);
    
    char *str = "hello lazy";
    
    printf("%ld\n", strlen(str));
    
    return 0;
}
