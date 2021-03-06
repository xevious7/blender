/*
 * Copyright 2011-2020 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <isa_availability.h>

/* The MS CRT defines this */
extern int __isa_available;

const char* get_arch_flags()
{
	if (__isa_available >= __ISA_AVAILABLE_AVX2) {
    return "/arch:AVX2";
  }
	if (__isa_available >= __ISA_AVAILABLE_AVX) {
    return "/arch:AVX";
  }
	return "";
}

int main()
{
    printf("%s\n", get_arch_flags());
    return 0;
}
