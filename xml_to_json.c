/*
** xml_to_json.c - 2018-01-22 - jakethaw
**
*************************************************************************
**
** MIT License
** 
** Copyright (c) 2019 jakethaw
** 
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
** copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions:
** 
** The above copyright notice and this permission notice shall be included in all
** copies or substantial portions of the Software.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
** SOFTWARE.
** 
*************************************************************************
** WebAssembly  *********************************************************
*************************************************************************
**
** To compile with Emscripten as a WebAssembly function:
**
**   emcc -Oz xml_to_json.c -o xml_to_json.js -s EXPORTED_FUNCTIONS='["_xml_to_json", "_free"]' -s 'EXTRA_EXPORTED_RUNTIME_METHODS=["allocate", "intArrayFromString", "ALLOC_NORMAL", "UTF8ToString"]'
**
*************************************************************************
**
** Usage example: 
**
** var xml = allocate(intArrayFromString("<x>hello world</x>"), 'i8', ALLOC_NORMAL);
** var indent = 2;
**
** var json = _xml_to_json(xml, indent);
** console.log(UTF8ToString(json, 5000));
**
** _free(xml);
** _free(json);
**
*************************************************************************
** SQLite3  *************************************************************
*************************************************************************
**
** Implementation of an SQLite3 xml_to_json(X, N) function.
** 
** xml_to_json(X, N) takes one or two arguments:
** 
** * X - XML string UTF-8 encoded
** * N - Optional indent for pretty printed JSON or -1 for minified JSON
** 
** The input XML is not validated prior to conversion.
**
*************************************************************************
**
** To compile with gcc as a run-time loadable extension:
**
**   UNIX-like : gcc -g -O2 -fPIC -shared xml_to_json.c -o xml_to_json.so -DSQLITE
**   Mac       : gcc -g -O2 -fPIC -dynamiclib xml_to_json.c -o xml_to_json.dylib -DSQLITE
**   Windows   : gcc -g -O2 -shared xml_to_json.c -o xml_to_json.dll -DSQLITE
**
** Add the -DDEBUG option to print debug information to stdout.
**
*************************************************************************
**
** Usage examples: 
**
** SELECT xml_to_json('<x>a<y/>b</x>');
** SELECT xml_to_json('<x><y>abc</y><y>def</y></x>', 2);
** SELECT xml_to_json('<x>hello<y>abc</y>world<y>def</y>xyz</x>', 2);
** SELECT xml_to_json('<x attr1="attr val 1" attr2="attr val 2">&amp; &gt; &lt; &#39;</x>', 2);
**
*************************************************************************
*/

#ifdef SQLITE
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#define MALLOC sqlite3_malloc
#define FREE sqlite3_free
#else
#define MALLOC malloc
#define FREE free
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct element *element;
struct element{
  struct element *parent;               // Link to parent element or null
  char *name;                           // Pointer to element name in original XML string
  int nName;                            // Length of name
  struct value *first_value;            // Link to first value. Value might be an array of values e.g <x>a<y/>b</x>
  int depth;                            // Depth of element
  int is_parent;                        // True if element has children
  int child_index;                      // Index of element among siblings
  int is_last_child;                    // True if element does not link to sibling
  int array_index;                      // Index of element in array
  int is_array_end;                     // True if last element in array
  struct element *next;                 // Link to next element. Sibling or ancestor's sibling
  struct element_attribute *first_attr; // Link to first attribute
};

typedef struct value *value;
struct value{
  struct value_part *first_value_part;  // Link to first value part
  struct value *next_value;             // Link to sibling value
};

// Divide value into parts to support special characters
// i.e.
//   &amp;  -> &
//   &gt;   -> >
//   &lt;   -> <
//   &quot; -> "
//   &apos; -> '
//   &#39;  -> '
//   etc.
//
//  Constant memory for named special charactes
//  Dynamically allocated memory for html codes values
//
typedef struct value_part *value_part;
struct value_part{
  char *val;                            // Pointer to value part in original XML string (or special characters)
  int nVal;                             // Length of val
  int free;                             // True if val should be freed
  struct value_part *next_value_part;   // Link to next value part
};

typedef struct element_attribute *element_attribute;
struct element_attribute{
  char *name;                           // Pointer to element name in original XML string
  int nName;                            // Lenth of name
  struct value_part *first_value_part;  // Link to first value part
  struct element_attribute *next_attr;  // Link to nect attribute
};

static value_part get_value_parts(int *i, int j, char *xml, value_part new_value_part, int is_attr);
static int json_output(element root, char *json, int indent);

static int is_space(char *z){
  return z[0]==' ' || z[0]=='\t' || z[0]=='\n' || z[0]=='\f' || z[0]=='\r';
}

static int print_spaces(char *json, int nJson, int spaces){
  if( spaces<0 )
    return 0;
  
  int i;
  for(i=0; i<spaces; i++){
    //printf(" ");
    if( json )
      json[nJson+i] = ' ';
  }
  
  return spaces;
}

static int print_newline(char *json, int nJson, int print){
  if( print<0 )
    return 0;
  
  //printf("\n");
  if( json )
    json[nJson] = '\n';
  
  return 1;
}

static int print_char(char *json, int nJson, char c){
  //printf("%c", c);
  if( json )
    json[nJson] = c;
  return 1;
}

static int print_string(char *json, int nJson, char *s, int n){
  //printf("%.*s", n, s);
  if( json )
    memcpy(&json[nJson], s, n);
  return n;
}

//
// xml_to_json
//
#ifdef SQLITE
static char *xml_to_json(char *xml, int indent){
#else
char *xml_to_json(char *xml, int indent){
#endif

  element root;
  element current_node = 0;
  element new_node;
  element parent_node;
  element previous_node;
  element test_node;
  element next_node;
  element previous_sibling;
  element previous_array_node;
  element test_node_deepest_node;
  
  element_attribute new_attr = 0;
  element_attribute current_attr = 0;
  element_attribute next_attr;
  
  value new_value;
  value current_value;
  value next_value;
  
  value_part new_value_part = 0;
  value_part current_value_part = 0;
  value_part next_value_part = 0;

  int i, j;
  int depth = 0;
  
  root = (element)MALLOC(sizeof(struct element));
  root->parent = 0;
  root->depth = 0;
  root->first_value = 0;
  root->is_parent = 0;
  root->child_index = 0;
  root->is_last_child = 1;
  root->array_index = 0;
  root->is_array_end = 0;
  root->next = 0;
  root->first_attr = 0;
  
  previous_node = root;
  
  i = 0;
  while( is_space(&xml[i]) ) i++;
  while(xml[i]){
    // Element open tag
    //printf("%.*s\n", 1, &xml[i]);
    if( xml[i]=='<' && xml[i+1]!='/' ){      
      // Create node
      depth++;
      new_node = (element)MALLOC(sizeof(struct element));
      
      // Node name
      j = 1;
      while( xml[i+j] && !is_space(&xml[i+j]) && !(xml[i+j]=='/' || xml[i+j]=='>') ) j++;
      j--;
      new_node->name = &xml[i+1];
      new_node->nName = j;
      i += j+1;
      
      // Default values
      new_node->first_value = 0;
      new_node->depth = depth;
      new_node->is_parent = 0;
      new_node->array_index = 0;
      new_node->is_array_end = 0;
      new_node->next = 0;
      new_node->child_index = 0;
      new_node->is_last_child = 0;
      new_node->first_attr = 0;
      
      // Set parent node
      parent_node = previous_node;
      while( parent_node->depth >= new_node->depth && parent_node->parent )
        parent_node = parent_node->parent;
      new_node->parent = parent_node;
      
      if( !parent_node->is_parent )
        parent_node->is_parent = 1;
      
      // Make new node the current node
      previous_node->next = new_node;
      previous_node = new_node;
      current_node = new_node;
      
      // printf("%.*s\n", j, current_node->name);
      // if( parent_node && parent_node->parent )
      // printf("  Parent = %.*s\n", parent_node->nName, parent_node->name);
      
      // Get attributes
      while( is_space(&xml[i]) ) i++;
      while( xml[i] && xml[i]!='/' && xml[i]!='?' && xml[i]!='>' ){
        // Create attribute
        new_attr = (element_attribute)MALLOC(sizeof(struct element_attribute));
        if( !current_node->first_attr ){
          current_node->first_attr = new_attr;
        }else{
          current_attr->next_attr = new_attr;
        }
        current_attr = new_attr;
        current_attr->first_value_part = 0;
        current_attr->next_attr = 0;
        
        // Attribute name
        j = 1;
        while( xml[i+j] && xml[i+j]!='=' && !is_space(&xml[i+j]) ) j++;
        current_attr->name = &xml[i];
        current_attr->nName = j;
        i += j;
        
        // Ensure attribute value starts
        while( xml[i] && (xml[i]!='"' || is_space(&xml[i])) ) i++;
        
        if( xml[i] ){
          i++;
          
          // Ensure attribute value ends
          j=0;
          while( xml[i+j] && xml[i+j]!='"' ) j++;
          
          if( xml[i+j] ){
            // Attribute value
            do{
              if( !current_attr->first_value_part ){
                new_value_part = (value_part)MALLOC(sizeof(struct value_part));
                new_value_part->next_value_part = 0; 
                current_attr->first_value_part = new_value_part;
              }else{
                new_value_part->next_value_part = (value_part)MALLOC(sizeof(struct value_part));
                new_value_part = new_value_part->next_value_part;
                new_value_part->next_value_part = 0;
              }

              new_value_part = get_value_parts(&i, 0, xml, new_value_part, 1);
            }while( xml[i] && xml[i]!='"' );
            
            if( xml[i] == '"' ){
              i++;
              while( is_space(&xml[i]) ) i++;
            }
          }
        }
      }
      
      // Self closing element
      if( xml[i]=='/' || xml[i]=='?' ){
        current_node = current_node->parent;
        depth--;
        while( xml[i] && xml[i]!='>' ) i++;
      }

    // Element close tag
    }else if( xml[i]=='<' && xml[i+1]=='/' ){
      current_node = current_node->parent;
      depth--;
      while( xml[i] && xml[i]!='>' ) i++;
      
    }else{
      i++;
      
      // Get value if it exists, or find the start of the next element
      j = 0;
      while( is_space(&xml[i+j]) ) j++;
      
      if( xml[i+j]!='<' || (!current_node->is_parent && xml[i+j]=='<' && xml[i+j+1]=='/') ){
        
        // Determine the deepest value of this element
        current_value = current_node->first_value;
        while( current_value && current_value->next_value )
          current_value = current_value->next_value;
        
        new_value = (value)MALLOC(sizeof(struct value));
        
        // Either make the new value the first value of the element,
        // or link the new value to the previous one
        if( !current_node->first_value ){
          current_node->first_value = new_value;
        }else{
          current_value->next_value = new_value;
        }
        
        new_value->first_value_part = 0;
        new_value->next_value = 0;

        // Value
        new_value_part = 0;
        while( xml[i] && xml[i]!='<' ){
          if( !new_value->first_value_part ){
            new_value_part = (value_part)MALLOC(sizeof(struct value_part));
            new_value_part->next_value_part = 0; 
            new_value->first_value_part = new_value_part;
          }else{
            new_value_part->next_value_part = (value_part)MALLOC(sizeof(struct value_part));
            new_value_part = new_value_part->next_value_part;
            new_value_part->next_value_part = 0;
          }
          new_value_part = get_value_parts(&i, 0, xml, new_value_part, 0);
          j = 0;
        }
        
        // if( new_value_part )
        //   printf("%.*s=%.*s\n", current_node->nName,
        //                         current_node->name,
        //                         new_value_part->nVal,
        //                         new_value_part->val);
      }
      i += j;
    }
  }
  
  //
  // Determine first/last nodes in a family
  //
  current_node = root;
  while(current_node->next){
    current_node = current_node->next;
    if( !current_node->child_index ){
      i = 1;
      test_node = current_node;
      previous_node = 0;
      do{
        if( current_node->parent == test_node->parent ){
          if( !current_node->child_index )
            current_node->child_index = 1;
          
          if( current_node != test_node )
            test_node->child_index = ++i;
          
          previous_node = test_node;
        }
        
        test_node = test_node->next;
      }while(test_node && test_node->depth >= current_node->depth );
      
      if( previous_node )
        previous_node->is_last_child = 1;
    }
  }
  
  //
  // Determine and group arrays
  //
  current_node = root;
  while(current_node->next){
    current_node = current_node->next;
    if( !current_node->array_index ){
      i = 1;
      test_node = current_node;
      previous_array_node = 0;
      while(test_node->next && test_node->depth >= current_node->depth){
        test_node = test_node->next;
        if( current_node->parent == test_node->parent 
            && current_node->nName == test_node->nName 
            && memcmp(current_node->name, test_node->name, test_node->nName) == 0){
          if( !current_node->array_index ){
            current_node->array_index = 1;
            previous_array_node = current_node; 
          }
          test_node->array_index = ++i;
          
          //
          // Re-order if array elements are separated
          //
          // e.g. <a>
          //        <b>1</b>
          //        <c/>
          //        <b>2</b>
          //      </a>
          //
          //      becomes:
          //
          //      <a>
          //        <b>1</b>
          //        <b>2</b>
          //        <c/>
          //      </a>
          //
          if( test_node->child_index != previous_array_node->child_index+1 ){
            
            next_node = previous_array_node->next;
            previous_sibling = next_node;

            //
            // Get the node that the furthest child of the test node points to
            //
            test_node_deepest_node = test_node;
            if( test_node_deepest_node->next ){
              while( test_node_deepest_node->next->depth > test_node->depth)
                test_node_deepest_node = test_node_deepest_node->next;
            }
            
            // Shift up each sibling node that sits between the previous array element and the test node
            while( next_node->next != test_node ){
              if( previous_array_node->parent == next_node->parent ){
                previous_sibling = next_node;
                previous_sibling->child_index++;
              }
              next_node = next_node->next;
            }
            if( previous_array_node->parent == next_node->parent ){
              previous_sibling = next_node;
              previous_sibling->child_index++;
            }
            
            // Set test node's previous node to point to the test node's next node
            next_node->next = test_node_deepest_node->next;
            
            // If the test node was the last child,
            // then flag the last array element as the last child,
            // and un-flag the test node
            if( test_node->is_last_child ){
              previous_sibling->is_last_child = 1;
              test_node->is_last_child = 0;
            }
            
            // Set the test node to be the next adjacent sibling to the 
            // previous array element
            test_node->child_index = previous_array_node->child_index+1;

            //
            // Get the node that the furthest child of the previous array node points to
            //
            next_node = previous_array_node;
            do{
              previous_node = next_node;
              next_node = next_node->next;
            }while( next_node->parent != previous_array_node->parent );
            
            previous_node->next = test_node;
            test_node_deepest_node->next = next_node;
          }
        
          previous_array_node = test_node;
        }
      }
      if( previous_array_node )
        previous_array_node->is_array_end = 1;
    }
  }
  
#ifdef DEBUG
  current_node = root;
  while(current_node->next){
    current_node = current_node->next;
   
    printf("%.*s\n", current_node->nName, current_node->name);
    if( current_node->parent && current_node->parent->parent )
      printf("  Parent = %.*s\n", current_node->parent->nName, current_node->parent->name);
    
    printf("  depth = %d\n", current_node->depth);
    printf("  is_parent = %d\n", current_node->is_parent);
    printf("  child_index = %d\n", current_node->child_index);
    printf("  is_last_child = %d\n", current_node->is_last_child);
    printf("  array_index = %d\n", current_node->array_index);
    printf("  is_array_end = %d\n", current_node->is_array_end);
    
    current_attr = current_node->first_attr;
    while( current_attr ){
      printf("  @%.*s=%.*s\n", current_attr->nName, current_attr->name, current_attr->nName, current_attr->name);
      current_attr = current_attr->next_attr;
    }
    
    printf("  \"#text\":\"%.*s\"\n", current_node->nName, current_node->name);
  }
#endif
  
   int nJson;
   char *json;

   // Calculate space required
   nJson = json_output(root, NULL, indent);
   
   // Construct JSON
   json = MALLOC(nJson+1);
   json_output(root, json, indent);
   json[nJson] = 0;
   
   // Cleanup elements
   current_node = root;
   next_node = current_node->next;
   while( current_node ){
     
     // Cleanup attributes
     current_attr = current_node->first_attr;
     while( current_attr ){
       
       // Cleanup value parts
       current_value_part = current_attr->first_value_part;
       while( current_value_part ){
         next_value_part = current_value_part->next_value_part;
         FREE(current_value_part);
         current_value_part = next_value_part;
       }
       
       next_attr = current_attr->next_attr;
       FREE(current_attr);
       current_attr = next_attr;
     }
     
     // Cleanup values
     current_value = current_node->first_value;
     while( current_value ){
       
       // Cleanup value parts
       current_value_part = current_value->first_value_part;
       while( current_value_part ){
         next_value_part = current_value_part->next_value_part;
         if( current_value_part->free ) FREE(current_value_part->val);
         FREE(current_value_part);
         current_value_part = next_value_part;
       }
       
       next_value = current_value->next_value;
       FREE(current_value);
       current_value = next_value;
     }
     
     next_node = current_node->next;
     FREE(current_node);
     current_node = next_node;
   }
   
   return json;
}

//
// html_code_to_str()
//
// Convert a html code to a char array.
//
//   e.g. &#39; to '
//
// must be freed
//
static void html_code_to_str(int *i, value_part value_part, const char *xml){
  // find end of html code
  int start = *i+1;
  int len = 0;
  while( xml[start+len]!=';' )
    len++;

  // advance through xml
  *i += 2+len;
  
  // str to int
  int m = 1; // multiplier 1, 10, 100 etc.
  unsigned long x = 0;
  while( len>0 ){
    x += (xml[start+len-1]-48)*m;
    m *= 10;
    len--;
  }

  // int to char array
  char *str;
  if( x < 1 << 8 ){
    value_part->nVal = 1;
    str = MALLOC(2);
    str[0] = x & 0xFF;
    str[1] = 0;
  }else if( x < 1 << 16 ){
    value_part->nVal = 2;
    str = MALLOC(3);
    str[0] = (x >> 8) & 0xFF;
    str[1] = x & 0xFF;
    str[2] = 0;
  }else if( x < 1 << 16 ){
    value_part->nVal = 3;
    str = MALLOC(4);
    str[0] = (x >> 16) & 0xFF;
    str[1] = (x >> 8) & 0xFF;
    str[2] = x & 0xFF;
    str[3] = 0;
  }else{
    value_part->nVal = 4;
    str = MALLOC(5);
    str[0] = (x >> 24) & 0xFF;
    str[1] = (x >> 16) & 0xFF;
    str[2] = (x >> 8) & 0xFF;
    str[3] = x & 0xFF;
    str[4] = 0;
  }
  value_part->free = 1;
  value_part->val = str;
}

static value_part get_value_parts(int *i, int j, char *xml, value_part new_value_part, int is_attr){

  while( xml[*i+j] && !(xml[*i+j]=='<'
                    || xml[*i+j]=='&'
                    || xml[*i+j]=='\b'
                    || xml[*i+j]=='\t'
                    || xml[*i+j]=='\n'
                    || xml[*i+j]=='\f'
                    || xml[*i+j]=='\r'
                    || xml[*i+j]=='"'
                    || xml[*i+j]=='\\') )
    j++;

  //printf("%.*s\n", j, &xml[*i]);
  
  new_value_part->nVal = j;
  new_value_part->val = &xml[*i];
  new_value_part->free = 0;
  *i += j;
  
  // Special characters
  if( xml[*i]=='&'
   || xml[*i]=='\b'
   || xml[*i]=='\t'
   || xml[*i]=='\n'
   || xml[*i]=='\f'
   || xml[*i]=='\r'
   || (xml[*i]=='"' && !is_attr)
   || xml[*i]=='\\' ){
    new_value_part->next_value_part = (value_part)MALLOC(sizeof(struct value_part));
    new_value_part = new_value_part->next_value_part;
    new_value_part->next_value_part = 0;
    new_value_part->free = 0;
  }
  
  if( xml[*i]=='&' ){
    *i += 1;
    if( memcmp("amp;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 1;
      new_value_part->val = "&";
      *i += 4;
    }else if( memcmp("gt;", &xml[*i], 3) == 0 ){
      new_value_part->nVal = 1;
      new_value_part->val = ">";
      *i += 3;
    }else if( memcmp("lt;", &xml[*i], 3) == 0 ){
      new_value_part->nVal = 1;
      new_value_part->val = "<";
      *i += 3;
    }else if( memcmp("quot;", &xml[*i], 5) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\\"";
      *i += 5;
    }else if( memcmp("apos;", &xml[*i], 5) == 0 ){
      new_value_part->nVal = 1;
      new_value_part->val = "'";
      *i += 5;
    }else if( memcmp("#8;", &xml[*i], 3) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\b";
      *i += 3;
    }else if( memcmp("#9;", &xml[*i], 3) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\t";
      *i += 3;
    }else if( memcmp("#10;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\n";
      *i += 4;
    }else if( memcmp("#12;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\f";
      *i += 4;
    }else if( memcmp("#13;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\r";
      *i += 4;
    }else if( memcmp("#34;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\\"";
      *i += 4;
    }else if( memcmp("#92;", &xml[*i], 4) == 0 ){
      new_value_part->nVal = 2;
      new_value_part->val = "\\\\";
      *i += 4;
    }else if( memcmp("#", &xml[*i], 1) == 0 ){
      html_code_to_str(i, new_value_part, (const char *)xml);
    }
  }else if( xml[*i]=='\b' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\b";
    *i += 1;
  }else if( xml[*i]=='\t' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\t";
    *i += 1;
  }else if( xml[*i]=='\n' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\n";
    *i += 1;
  }else if( xml[*i]=='\f' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\f";
    *i += 1;
  }else if( xml[*i]=='\r' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\r";
    *i += 1;
  }else if( !is_attr && xml[*i]=='"' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\\"";
    *i += 1;
  }else if( xml[*i]=='\\' ){
    new_value_part->nVal = 2;
    new_value_part->val = "\\\\";
    *i += 1;
  }

  return new_value_part;
}

#define PRINT_SPACES(x) nJson += print_spaces(json, nJson, x)
#define PRINT_NEWLINE nJson += print_newline(json, nJson, indent)
#define PRINT_CHAR(x) nJson += print_char(json, nJson, x)
#define PRINT_STRING(z,n) nJson += print_string(json, nJson, z, n);

//
// json_output
//
// If *json is null, then return total space required.
// If *json is not null, then populate with JSON string.
//
// Does not zero terminate JSON string.
//
int json_output(element root, char *json, int indent){
  int nJson = 0;
  int depth = 0;
  
  element current_node;
  element parent_node;
  element_attribute current_attr;
  value current_value;
  value_part current_value_part;

  current_node = root;
  
  while(current_node->next){
    current_node = current_node->next;  

    // Opening bracket
    if( (current_node->child_index == 1 && !current_node->parent->first_attr && !current_node->parent->first_value) || current_node == root->next ){
      if( current_node->parent->array_index > 1){
        PRINT_SPACES(depth*indent);
      }
      PRINT_CHAR('{');
      PRINT_NEWLINE;
      depth++;
    }
    
    // Node name
    if( current_node->array_index <= 1 ){
      PRINT_SPACES(depth*indent);
      PRINT_CHAR('"');
      PRINT_STRING(current_node->name, current_node->nName);
      PRINT_CHAR('"');
      PRINT_CHAR(':');
      PRINT_SPACES(indent < 0 ? 0 : 1);
    }
    
    // Attributes
    current_attr = current_node->first_attr;
    if( current_attr ){
      
      if( current_node->array_index == 1 ){
        depth++;
        PRINT_CHAR('[');
        PRINT_NEWLINE;
      }
      
      if( current_node->array_index ){
        PRINT_SPACES(depth*indent);
      }
      
      PRINT_CHAR('{');
      PRINT_NEWLINE;
      depth++;
      
      while(current_attr){
        // "@name":"value",
        PRINT_SPACES(depth*indent);
        PRINT_CHAR('"');
        PRINT_CHAR('@');
        PRINT_STRING(current_attr->name, current_attr->nName);
        PRINT_CHAR('"');
        PRINT_CHAR(':');
        PRINT_SPACES(indent < 0 ? 0 : 1);

        // Join value parts
        PRINT_CHAR('"');
        current_value_part = current_attr->first_value_part;
        while( current_value_part ){
          PRINT_STRING(current_value_part->val, current_value_part->nVal);
          current_value_part = current_value_part->next_value_part;
        }
        PRINT_CHAR('"');

        current_attr = current_attr->next_attr;
        
        if( current_attr || current_node->first_value || current_node->is_parent ){
          PRINT_CHAR(',');
          PRINT_NEWLINE;
        }
      }
      
      if( !current_node->first_value && !current_node->is_parent ){
        depth--;
        PRINT_NEWLINE;
        PRINT_SPACES(depth*indent);
        PRINT_CHAR('}');
      }
    }
    
    // #text
    if( current_node->first_value && (current_node->first_attr || current_node->is_parent) ){
      if( current_node->array_index ){
        PRINT_SPACES(depth*indent);
      }
      if( current_node->is_parent && !current_node->first_attr ){
        PRINT_CHAR('{');
        PRINT_NEWLINE;
        depth++;
      }
      if( !(current_node->first_attr && current_node->array_index ) ){
        PRINT_SPACES(depth*indent);
      }
      PRINT_STRING("\"#text\":", 8);
      PRINT_SPACES(indent < 0 ? 0 : 1);
      
      // Array of values
      if( current_node->first_value->next_value ){
        PRINT_CHAR('[');
        PRINT_NEWLINE;
        current_value = current_node->first_value;
        
        while( current_value ){
          PRINT_SPACES((depth+1)*indent);
          
          // Join value parts
          PRINT_CHAR('"');
          current_value_part = current_value->first_value_part;
          while( current_value_part ){
            PRINT_STRING(current_value_part->val, current_value_part->nVal);
            current_value_part = current_value_part->next_value_part;
          }
          PRINT_CHAR('"');

          current_value = current_value->next_value;
          if( current_value ){
            PRINT_CHAR(',');
            PRINT_NEWLINE;
          }else{
            PRINT_NEWLINE;
            PRINT_SPACES(depth*indent);
            PRINT_CHAR(']');
          }
        }
      }
    }
    
    // Array start
    if( current_node->array_index == 1 && !current_node->first_attr ){
      depth++;
      PRINT_CHAR('[');
      PRINT_NEWLINE;
      if( current_node->is_parent ){
        PRINT_SPACES(depth*indent);
      }
    }
    
    // null
    if( !current_node->first_value && !current_node->is_parent && !current_node->first_attr ){
      if( current_node->array_index ){
        PRINT_SPACES(depth*indent); 
      }
      PRINT_STRING("null", 4);
    }
    
    // Value
    if( current_node->first_value && !current_node->first_value->next_value ){
      if( current_node->array_index && !current_node->is_parent && !current_node->first_attr ){
        PRINT_SPACES(depth*indent);
      }
      
      // Join value parts
      PRINT_CHAR('"');
      current_value_part = current_node->first_value->first_value_part;
      while( current_value_part ){
        PRINT_STRING(current_value_part->val, current_value_part->nVal);
        current_value_part = current_value_part->next_value_part;
      }
      PRINT_CHAR('"');
      
      if( current_node->first_attr && !current_node->is_parent ){
        depth--;
        PRINT_NEWLINE;
        PRINT_SPACES(depth*indent);
        PRINT_CHAR('}');
      }
    }
    
    // Comma
    if( (!current_node->is_last_child && !current_node->is_array_end && !current_node->is_parent) || (current_node->is_parent && current_node->first_value) ){
      PRINT_CHAR(',');
      PRINT_NEWLINE;
    }
    
    // Trailing brackets
    if( (current_node->is_last_child || current_node->is_array_end ) && !current_node->is_parent ){
      parent_node = current_node;
      
      while( parent_node != root && (!current_node->next || parent_node != current_node->next->parent) ){
        if( parent_node->is_array_end ){
          depth--;
          PRINT_NEWLINE;
          PRINT_SPACES(depth*indent);
          PRINT_CHAR(']');
          if( !parent_node->is_last_child ){
            PRINT_CHAR(',');
          }
        }
        
        if( parent_node->is_last_child ){
          depth--;
          PRINT_NEWLINE;
          PRINT_SPACES(depth*indent);
          PRINT_CHAR('}');
          if( !parent_node->parent->is_last_child && !parent_node->parent->is_array_end ){
            PRINT_CHAR(',');
          }
        }

        parent_node = parent_node->parent;
      }
      PRINT_NEWLINE;
    }
    
  }
  
  return nJson;
}

#ifdef SQLITE
/*
** Implementation of xml_to_json() function.
*/
static void xml_to_jsonFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  if( sqlite3_value_type(argv[0])==SQLITE_NULL ) return;
  int indent = -1;
  char *xml = (char *)sqlite3_value_text(argv[0]);
  char *json;
	  
  if( argc==2 ){
    if( sqlite3_value_type(argv[1])!=SQLITE_NULL )
      indent = sqlite3_value_int(argv[1]);
  }
  
  json = xml_to_json(xml, indent);
  
  sqlite3_result_text(context, json, -1, sqlite3_free);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_xmltojson_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = sqlite3_create_function(db, "xml_to_json", 1, SQLITE_UTF8, 0,
                               xml_to_jsonFunc, 0, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "xml_to_json", 2, SQLITE_UTF8, 0,
                                 xml_to_jsonFunc, 0, 0);
  }
  return rc;
}
#endif
