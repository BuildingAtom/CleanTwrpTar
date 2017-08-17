#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define BLOCK_SIZE 512
#define BUFFER_SIZE 1024
#define TAR_EOF

//this value determines the max block size used for copying the file. a value of 64 will translate to 32k.
#define COPY_BLOCK_SIZE 64

typedef struct {
    char ustar[6];
    char ustarVersion[2];
    char ownerName[32];
    char groupName[32];
    char devMajor[8];
    char devMinor[8];
    char filenamePrefix[155];
} ustar_header_t;

typedef struct {
    char filename[100];
    char filemode[8];
    char ownerID[8];
    char groupID[8];
    char filesize[12];
    char time[12];
    char checksum[8];
    char filetype;
    char linkedFile[100];
    ustar_header_t ustar_header;
} tar_header_t;

typedef struct {
    int last_header_bpos;
    long last_filesize;
    int last_number_of_blocks;
    int header_bpos;
    int offset;
    long filesize;
    int number_of_blocks;
    int next_header_bpos;
} header_info_t;

// Known Leaked Strings
const char *leaked_strings[] = { "storing xattr user.default\n",
    "storing xattr user.inode_cache\n", "storing xattr user.inode_code_cache\n", NULL };

// Check for these specific leaked strings towards the end of the file. (last 2048 bytes)
const char *leaked_strings_end[] = { "I:Closing tar\n", NULL };

// Additional Strings Stuff (note these strings are not null terminated!)
typedef struct {
    uint8_t length;
    char *string;
} additional_strings_t;

// And pointer
additional_strings_t **additional_strings;

// Additional Strings File Header (todo, define header size in preprocessor)
const char strings_file_header[8] = {'T','C','9','9','\0','\0','\0','\n'};
#define STRINGS_FILE_SPLIT ':'
#define STRINGS_FILE_END ';'

// Help Message.
#define HELP_MESSAGE "this is the help message \n"

//Variables and flags
bool verbose = false; // -v --verbose
bool ask_in_files = false; // -a --ask
bool ignore_in_files = false; // -i --ignore
bool search_for_new_strings = false; // -n --search-for-new
FILE *input_file = NULL; //first argument
FILE *output_file = NULL; //second argument
FILE *strings_file = NULL; //-s [file] --strings-file

//Used when an overall offset is required (in bytes).
int global_offset = 0;

// Positioning numbers (in blocks)
int input_bpos = 0;
int output_bpos = 0;

//header "indexing" (not really)
header_info_t header_info = {-1, -1, -1, -1, -1, -1, -1, 0};

//found strings in files
typedef struct {
    unsigned int start_bpos;
    uint8_t length;
} string_clusters_t;

string_clusters_t **found_strings;
string_clusters_t **new_strings;
string_clusters_t **end_strings;

//io buffer
char buffer[BUFFER_SIZE];
char copy_buffer[BLOCK_SIZE*COPY_BLOCK_SIZE];

// The actual clean function
int cleanTwrpTar();
// Identify the offset of the next header in the file.
int nextHeaderOffset();
// Parse the provided strings file. Returns the number of strings.
int parseStringsFile();
// Adds a string to the strings file if provided and adds it to the additional strings. Returns new number of strings.
int addNewString(char *string_to_add, uint8_t length);
// Search for known strings in between data block before the header and the header block. Returns size of strings found.
int preHeaderCheck();
// Search for known strings in data file of header (between block boundaries). Returns number of string clusters found. Needs the size found so far.
int searchKnownsFile(int size_found);
// Search for possible new strings. For now it just looks at the space before the header.
int searchNewStrings(int size_found);
// For the end of the file, perform this function to check and everything. It will check any specific end strings
int endFileCheck();
// Copy from input file to output file given starting block position, ending block position (copied), and offset of start
int copyBlocks(int start_bpos, int end_bpos, int block_offset);

int main(int argc, char **argv)
{
    char *input_filepath = NULL; //first argument
    char *output_filepath = NULL; //second argument
    char *strings_filepath = NULL; //-s [file] --strings-file
    /*
    //verify argv[0] is the executable
    printf("%s\n", argv[0]);
    */

    // Print the help message and return the program if no arguments provided
    if (argc < 2) {
        puts(HELP_MESSAGE);
        return 0;
    }

    for (int i = 1; i<argc; i++){
        //if help message is asked for, print help and exit.
        if (strcmp(argv[i], "--help") == 0){
            puts(HELP_MESSAGE);
            return 0;
        }

        //otherwise build the actions
        if (argv[i][0] == '-' && argv[i][1] == '-'){
            if (strcmp(argv[i]+2, "verbose") == 0){
                verbose = true;
                continue;
            }
            if (strcmp(argv[i]+2, "ask") == 0){
                ask_in_files = true;
                continue;
            }
            if (strcmp(argv[i]+2, "ignore") == 0){
                ignore_in_files = true;
                continue;
            }
            if (strcmp(argv[i]+2, "search-for-new") == 0){
                search_for_new_strings = true;
                continue;
            }
            if (strcmp(argv[i]+2, "strings-file") == 0){
                if (strings_filepath != NULL){
                    printf("Error: Strings file already defined\n");
                    return 1;
                }
                if (++i >= argc) {
                    printf("Error: Strings file not provided\n");
                    return 1;
                }
                strings_filepath = argv[i];
                continue;
            }
            printf("Error: %s is not a valid argument\n\n", argv[i]);
            puts(HELP_MESSAGE);
            return 1;
        }

        //Separated for readability
        else if (argv[i][0] == '-'){
            bool declare_strings_file = false;
            for (int x = 0; x < 5; x++){
                switch (argv[i][1+x]){
                case 'v':
                    verbose = true;
                    break;
                case 'a':
                    ask_in_files = true;
                    break;
                case 'i':
                    ignore_in_files = true;
                    break;
                case 'n':
                    search_for_new_strings = true;
                    break;
                case 's':
                    declare_strings_file = true;
                    break;
                case '\0':
                    if (!x){
                        printf("Error: Invalid argument '-'\n\n");
                        puts(HELP_MESSAGE);
                        return 1;
                    }
                    x = 5;
                    break;
                default:
                    printf("Error: Invalid argument '-%c'\n\n", argv[i][1+x]);
                    puts(HELP_MESSAGE);
                    return 1;
                }
            }
            if (declare_strings_file){
                if (strings_filepath != NULL){
                    printf("Error: Strings file already defined\n");
                    return 1;
                }
                if (++i >= argc) {
                    printf("Error: Strings file not provided\n");
                    return 1;
                }
                strings_filepath = argv[i];
            }
            continue;
        }

        //check for input file argument, and set the pointer to the same address
        else if (input_filepath == NULL){
            input_filepath = argv[i];
        }

        //check for output file argument, and set the pointer to the same address
        else if (output_filepath == NULL){
            output_filepath = argv[i];
        }
        else {
            printf("Error: Too many arguments.\n\n");
            puts(HELP_MESSAGE);
            return 1;
        }
    }

    // Attempt to open the input file
    input_file = fopen(input_filepath, "rb");
    if (input_file == NULL){
        printf("Error: Failed to open input file %s\n", input_filepath);
        return 1;
    }
    // Set buffer to NULL for to reduce repeated reads (alternately I could set a larger buffer, and possibly access the buffer for some functions instead)
    setbuf(input_file, NULL);

    // Attempt to open the output file
    output_file = fopen(output_filepath, "wb+");
    if (output_file == NULL){
        printf("Error: Failed to open output file %s. Check to see if you have write permissions or if the file already exists.\n", output_filepath);
        return 1;
    }

    // Check if we should open the strings file
    if (strings_filepath != NULL){
        // Attempt to open the strings file
        strings_file = fopen(strings_filepath, "ab+");
        if (strings_file == NULL){
            printf("Error: Failed to open strings file %s\n", strings_filepath);
            return 1;
        }
    } else if (verbose) printf("Proceeding without strings file.\n");

    //parse the strings file
    parseStringsFile(additional_strings);

    //begin working
    return cleanTwrpTar();
}

int cleanTwrpTar()
{
    //printf("sizeof = %d\n", sizeof(tar_header_t));
    /*int offset = nextHeaderOffset();
    if (offset < 0){
        printf("Error: Unexpected end of file within 1024 bytes. Exiting.\n");
        return 1;
    }
    if (offset > 0){
        int found_size = preHeaderCheck();
        if (found_size != offset){
            //check for new string before header
        }
    }
    //now begin the search
    //while (!((offset = nextHeaderOffset()) < 0));
    //copyBlocks(0, 1, 0);*/

    //addNewString("storing xattr user.default\n", 27);

    int offset = 0;
    while ((offset = nextHeaderOffset()) >= 0){
        if (offset == 0){
            int blocks_to_copy = header_info.header_bpos-header_info.last_header_bpos;
            int copied = copyBlocks(header_info.last_header_bpos+1, header_info.header_bpos, 0);
            if (blocks_to_copy != copied) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);
            continue;
        }
        int size_found_pre_header = preHeaderCheck();
        int string_clusters = searchKnownsFile(size_found_pre_header);
        //NOTE: I could simplify this a bit if I had the headercheck also throw into the stringclusters, but I don't feel like it rn
        //this isn't the final cleanup code, but it works for now.
        int current_bpos = header_info.last_header_bpos+1;
        int total_found_inside = 0;
        for (int i = 0; i<string_clusters; i++){
            int blocks_to_copy = found_strings[i]->start_bpos - current_bpos;
            //edge case for beginning
            if (blocks_to_copy == 0){
                total_found_inside += found_strings[i]->length;
                continue;
            }
            int copied = copyBlocks(current_bpos, found_strings[i]->start_bpos-1, total_found_inside);
            if (blocks_to_copy != copied) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);

            current_bpos = found_strings[i]->start_bpos;

            total_found_inside += found_strings[i]->length;
            printf("start_bpos: %d\tlength:%d\n", found_strings[i]->start_bpos, found_strings[i]->length);
        }
        if (size_found_pre_header != 0){
            int blocks_to_copy = header_info.header_bpos-current_bpos;
            int copied = copyBlocks(current_bpos, header_info.header_bpos-1, total_found_inside);
            if (blocks_to_copy != copied) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);
            current_bpos = header_info.header_bpos;
        }

        int blocks_to_copy = header_info.header_bpos-current_bpos+1;
        int copied = copyBlocks(current_bpos, header_info.header_bpos, header_info.offset);
        if (copied != blocks_to_copy) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);

        printf("Block Position: %d\tNext: %d\tSize: %d (%d blocks)\nOffset: %d\tGlobal Offset: %d\nSize of strings found in before header: %d\nString clusters found in file: %d (Size: %d)\nNew string stringcluster size: %d\n\n",
               header_info.header_bpos, header_info.next_header_bpos, header_info.filesize, header_info.number_of_blocks, header_info.offset, global_offset, size_found_pre_header,
               string_clusters, total_found_inside, searchNewStrings(size_found_pre_header+total_found_inside));
    }
    if (offset == -1){
        printf("Assumed Proper EOF. Performing End File Check.\n");
        int string_clusters = endFileCheck();

        //since now header_info refers to the last header before the end, and header_info.next_header_bpos should refer to the 2 NULL blocks for EOF
        int current_bpos = header_info.header_bpos+1;
        int total_found_inside = 0;
        for (int i=0; i<string_clusters; i++){
            int blocks_to_copy = end_strings[i]->start_bpos - current_bpos;
            //edge case for beginning
            if (blocks_to_copy == 0){
                total_found_inside += end_strings[i]->length;
                continue;
            }
            int copied = copyBlocks(current_bpos, end_strings[i]->start_bpos-1, total_found_inside);
            if (blocks_to_copy != copied) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);

            current_bpos = end_strings[i]->start_bpos;
            total_found_inside += end_strings[i]->length;
        }

        int blocks_to_copy = header_info.next_header_bpos-current_bpos+2;
        int copied = copyBlocks(current_bpos, header_info.next_header_bpos+1, total_found_inside);
        if (copied != blocks_to_copy) printf("Copied %d blocks of %d\n", copied, blocks_to_copy);

        printf("Found %d string clusters (Size: %d)\n", string_clusters, total_found_inside);
        return 0;
    }
    if (offset == -2){
        printf("Possibly Improper EOF. Block Position of last header is %d.", header_info.header_bpos);
        return -1;
    }


    return 0;
}

//Scan for the next header (assuming offset can never be negative).
//Returns -1 if most likely proper EOF, -2 if possibly improper, otherwise offset of next header.
int nextHeaderOffset()
{
    int read_size = 0;
    // this pointer moves as needed
    char *tar_header = buffer;
    // these two variables in combination are later used to calculate header offset
    short header_offset = 0;
    int header_offset_blocks = 0;
    /* to implement if I ever feel like it in the future (read retries)
    bool read_successful = false;
    short read_tries = 0;
    */

    //check if we're looking for the first header
    if (header_info.header_bpos == -1){
        rewind(input_file);
        read_size = fread(buffer, 1, BLOCK_SIZE*2, input_file);
        input_bpos = 0;
    } else {
        // update global offset in case not zero
        if (header_info.offset > 0) global_offset += header_info.offset;
        // seek to the expected location of the next header and read.
        fseek(input_file, (header_info.next_header_bpos)*BLOCK_SIZE + global_offset, SEEK_SET);
        read_size = fread(buffer, 1, BLOCK_SIZE*2, input_file);
        input_bpos = header_info.next_header_bpos;
    }
    if (read_size != BLOCK_SIZE*2) {
        if (feof(input_file)){
            printf("Warning: Unexpected end of file. Filling in null bytes and closing file.\n");
            return -2;
        }else{
            printf("Reading error\n");
            exit(1);
        }
    }

    // look for next ustar. Run loop while ustar is not ustar, it's not EOF, and read size is less than buffer size
    while (memcmp( ((tar_header_t*)tar_header)->ustar_header.ustar, "ustar", 5 ) != 0){// && !feof() && read_size < BUFFER_SIZE )
        // separated for readability
        // NOTE FOR SELF: THERE MIGHT BE AN OFF BY ONE ERROR HERE (UGH)
        header_offset++;
        // I included this case scenario, but I really hope no one ever needs
        // to go here. Something must've gone terribly wrong if they have (except for EOF)
        if (header_offset > BLOCK_SIZE){
            header_offset = 0;
            header_offset_blocks++;

            //rewind a bit
            fseek(input_file, (++input_bpos)*BLOCK_SIZE + global_offset, SEEK_SET); //preincrement
            read_size = fread(buffer, 1, BLOCK_SIZE*2, input_file);
            if (read_size != BLOCK_SIZE*2) {
                if (feof(input_file)){
                    // this is likely the proper EOF, so rewind and return -1 to signal ending of file.
                    rewind(input_file);
                    input_bpos = 0;
                    return -1;
                }else{
                    printf("Reading error\n");
                    exit(1);
                }
            }

            tar_header = buffer;
            continue;
        }
        tar_header++;
    }

    //update the header data structure.
    header_info.last_header_bpos = header_info.header_bpos;
    header_info.last_filesize = header_info.filesize;
    header_info.last_number_of_blocks = header_info.number_of_blocks;
    header_info.header_bpos = header_info.next_header_bpos;
    header_info.offset = header_offset_blocks*BLOCK_SIZE+header_offset;
    header_info.filesize = strtol( ((tar_header_t*)tar_header)->filesize, NULL, 8);
    header_info.number_of_blocks = (int)(header_info.filesize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    header_info.next_header_bpos = header_info.header_bpos+1+header_info.number_of_blocks;

    return header_info.offset;
}

// Parse the strings file and return the number of additional strings
int parseStringsFile()
{
    if (strings_file == NULL) return 0;

    //size of string in hexadecimal
    char size[3] = {'\0'};
    int num_of_strings = 0;
    void *temp_pointer;

    rewind(strings_file);
    int c = fgetc(strings_file);
    if (c == EOF){
        //file is empty, insert header and call it a day
        int written = fwrite(strings_file_header, sizeof(char), sizeof(strings_file_header), strings_file);
        if (verbose) printf("Provided strings file appears to be empty. Attempting to create new strings file...\n");
        if (written != sizeof(strings_file_header)){
            printf("Warning: Error creating strings file. Proceeding without strings file.\n");
            fclose(strings_file);
            strings_file = NULL;
        }
        return 0;
    }

    //check the header
    for (int i = 0; i < sizeof(strings_file_header); i++){
        if ((char)c != strings_file_header[i]){
            printf("Error: Strings file does not appear to be for this program (bad header).\n");
            exit(1);
        }
        c = fgetc(strings_file);
    }

    //the above already pulled the next character
    while (c != EOF){
        //update size of next string
        size[0] = c;
        size[1] = fgetc(strings_file);
        //allocate for the pointer to the pointer
        temp_pointer = realloc(additional_strings, (++num_of_strings+1)*sizeof(additional_strings_t*)); //notice the pre-increment
        if (temp_pointer == NULL){  //expanded for better understanding
            printf("Error: Unable to (re)allocate memory.\n");
            exit(1);
        }
        additional_strings = (additional_strings_t**)temp_pointer;
        //then allocate for the actual structure
        additional_strings[num_of_strings-1] = malloc(num_of_strings*sizeof(additional_strings_t)); //notice the LACK of pre-increment
        if (additional_strings[num_of_strings-1] == NULL){  //expanded for better understanding
            printf("Error: Unable to allocate memory.\n");
            exit(1);
        }
        additional_strings[num_of_strings-1]->length = (uint8_t)strtol(size, NULL, 16); //notice the -1

        //Verify structure matches
        if (fgetc(strings_file) != STRINGS_FILE_SPLIT){
            printf("Error: Strings file does not appear to be for this program or is corrupt (bad data).\n");
            exit(1);
        }

        //then pull it
        //allocate memory for the string
        additional_strings[num_of_strings-1]->string = malloc(additional_strings[num_of_strings-1]->length*sizeof(char));
        if (additional_strings[num_of_strings-1]->string == NULL){
            printf("Error: Unable to (re)allocate memory.\n");
            exit(1);
        }
        //and store it (note these strings are not null terminated!)
        for (int i = 0; i < additional_strings[num_of_strings-1]->length; i++){
            additional_strings[num_of_strings-1]->string[i] = fgetc(strings_file);
            if (feof(strings_file)){
                printf("Error: Strings file does not appear to be for this program or is corrupt (bad data).\n");
                exit(1);
            }
        }

        //Verify structure matches
        if (fgetc(strings_file) != STRINGS_FILE_END){
            printf("Error: Strings file does not appear to be for this program or is corrupt (bad data).\n");
            exit(1);
        }


        //ensure that there is a null pointer at the end of the array.
        additional_strings[num_of_strings] = NULL;

        //update the next character for the while loop
        c = fgetc(strings_file);
    }
    return num_of_strings;
}

//Store string into strings file as well as add to current list of strings in memory
int addNewString(char *string_to_add, uint8_t length)
{
    void *temp_pointer;
    if (strings_file != NULL){
        if (verbose) printf("Adding string to strings file.\n");
        fprintf(strings_file, "%X:", length);
        fwrite(string_to_add, sizeof(char), length, strings_file);
        if (fputc(STRINGS_FILE_END, strings_file) == EOF){
            printf("Warning: Error writing to strings file. Proceeding without strings file.\n");
            fclose(strings_file);
            strings_file = NULL;
        }
        fflush(strings_file);
    }

    //find number of strings
    int num_of_strings = 0;
    if (additional_strings != NULL)
        while (additional_strings[num_of_strings] != NULL) num_of_strings++;

    //add to existing strings (copied with some modifications from above)
    //allocate for pointer to pointer
    temp_pointer = realloc(additional_strings, (++num_of_strings+1)*sizeof(additional_strings_t*)); //notice the pre-increment
    if (temp_pointer == NULL){  //expanded for better understanding
        printf("Warning: Unable to (re)allocate memory. Proceeding without new string.\n");
        //undo
        return --num_of_strings;
    }
    additional_strings = (additional_strings_t**)temp_pointer;
    //then allocate for structure
    additional_strings[num_of_strings-1] = malloc(num_of_strings*sizeof(additional_strings_t)); //notice the LACK of pre-increment
    if (additional_strings[num_of_strings-1] == NULL){  //expanded for better understanding
        printf("Warning: Unable to allocate memory. Proceeding without new string.\n");
        //render the additional pointer useless.
        additional_strings[--num_of_strings] = NULL;
        return num_of_strings;
    }
    additional_strings[num_of_strings-1]->length = length; //notice the -1

    //then copy it
    //allocate memory for the string
    additional_strings[num_of_strings-1]->string = malloc(additional_strings[num_of_strings-1]->length*sizeof(char));
    if (additional_strings[num_of_strings-1]->string == NULL){
        printf("Error: Unable to allocate memory. Proceeding without new string.\n");
        //render the additional length stored already useless.
        additional_strings[--num_of_strings] = NULL;
        return num_of_strings;
    }
    //and store it (note these strings are not null terminated!)
    for (int i = 0; i < length; i++){
        additional_strings[num_of_strings-1]->string[i] = string_to_add[i];
    }
    additional_strings[num_of_strings] = NULL;
    return num_of_strings;
}

// returns size of strings found between the current header and the previous data block (if there's any, otherwise file start)
// should always return either an equal value or less than the offset, unless something went terribly wrong
int preHeaderCheck()
{
    //if there's no offset to the next header, there are no strings to be found, so return 0
    if (header_info.offset == 0) return 0;

    int sizeFound = 0;

    //load offset area of header into memory
    //because I was too lazy to check for offsets larger than 1024 bytes, the limit offset is going to be 1024 bytes.
    if (input_bpos != header_info.header_bpos){
        fseek(input_file, (header_info.header_bpos)*BLOCK_SIZE + global_offset, SEEK_SET);
        int read_size = fread(buffer, 1, BLOCK_SIZE*2, input_file);
        if (read_size != BLOCK_SIZE*2) {
            printf("Reading error\n");
            exit(1);
        }
        input_bpos = header_info.header_bpos;
    }

    //because I didn't want to make a separate function for a more elegant, recursive solution
    int last_sizeFound = -1;
    while (last_sizeFound != sizeFound){
        last_sizeFound = sizeFound;
        for (int i = 0; leaked_strings[i] != NULL; i++){
            int temp_stringlen = strlen(leaked_strings[i]);
            //see if string is too big to be in the offset area
            if (temp_stringlen + sizeFound > header_info.offset) continue;

            //check for string in reverse
            for (int x = 1; x <= temp_stringlen; x++)
                if (header_info.offset-sizeFound-x < 0 || buffer[header_info.offset-sizeFound-x] != leaked_strings[i][temp_stringlen-x]){
                    temp_stringlen = 0; //string not found
                    break;
                }

            sizeFound += temp_stringlen;
            if (temp_stringlen != 0) break; //reset loop
        }

        if (additional_strings == NULL || last_sizeFound != sizeFound) continue;

        for (int i = 0; additional_strings[i] != NULL; i++){
            uint8_t temp_stringlen = additional_strings[i]->length;
            //see if string is too big to be in the offset area
            if (temp_stringlen + sizeFound > header_info.offset) continue;

            //check for string in reverse
            for (int x = 1; x <= temp_stringlen; x++)
                if (header_info.offset-sizeFound-x < 0 || buffer[header_info.offset-sizeFound-x] != additional_strings[i]->string[temp_stringlen-x]){
                    temp_stringlen = 0; //string not found
                    break;
                }

            sizeFound += temp_stringlen;
            if (temp_stringlen != 0) break; //reset loop
        }
    }
    return sizeFound;
}

//search inside the file for any known strings.
int searchKnownsFile(int size_found)
{
    if (found_strings != NULL) {
        // free the memory from the previous found strings
        for (int i = 0; found_strings[i]; i++)
            free(found_strings[i]);

        free(found_strings);
        found_strings = NULL;
    }

    int search_size = header_info.offset - size_found;

    // don't actually run the function if there's nothing to search for, if we're on the first header, or if we're ignoring in files.
    if (search_size <= 0 || header_info.last_header_bpos < 0 || ignore_in_files) return 0;

    //offset from blocks as searching
    int offset = 0;
    void *temp_pointer;
    int stringclusters_found = 0;

    //start checking the beginning of every block
    for (int i = 0; i < header_info.last_number_of_blocks && offset != search_size; i++){
        //load offset area of header into memory
        //Even better than before! The limit is 512 bytes!
        if (input_bpos != header_info.last_header_bpos+i+1){
            fseek(input_file, (header_info.last_header_bpos+i+1)*BLOCK_SIZE + global_offset + offset, SEEK_SET);
            int read_size = fread(buffer, 1, BLOCK_SIZE, input_file);
            if (read_size != BLOCK_SIZE) {
                printf("Reading error\n");
                exit(1);
            }
            input_bpos = header_info.last_header_bpos+i+1;
        }

        //check for strings
        //search loop like above
        int searchoffset = 0;
        int last_searchoffset = -1;
        while (last_searchoffset != searchoffset){
            last_searchoffset = searchoffset;
            for (int i = 0; leaked_strings[i] != NULL; i++){
                int temp_stringlen = strlen(leaked_strings[i]);
                //see if string is too big to be in the search area
                if (temp_stringlen + searchoffset + offset > search_size) continue;

                //check for string
                for (int x = 0; x < temp_stringlen; x++)
                    if (buffer[x+searchoffset] != leaked_strings[i][x]){
                        temp_stringlen = 0; //string not found
                        break;
                    }

                //add to the sizes found
                searchoffset += temp_stringlen;

                if (temp_stringlen != 0) break; //reset loop
            }

            if (additional_strings == NULL || last_searchoffset != searchoffset) continue;

            for (int i = 0; additional_strings[i] != NULL; i++){
                uint8_t temp_stringlen = additional_strings[i]->length;
                //see if string is too big to be in the search area
                if (temp_stringlen + searchoffset + offset > search_size) continue;

                //check for string in reverse
                for (int x = 0; x < temp_stringlen; x++)
                    if (buffer[x+searchoffset] != additional_strings[i]->string[x]){
                        temp_stringlen = 0; //string not found
                        break;
                    }

                //add to the sizes found
                searchoffset += temp_stringlen;

                if (temp_stringlen != 0) break; //reset loop
            }
        }

        //continue if nothing found
        if (searchoffset == 0) continue;

        //otherwise add to the list of found strings
        temp_pointer = realloc(found_strings, (++stringclusters_found+1)*sizeof(string_clusters_t*)); //preincrement
        if (temp_pointer == NULL){
            printf("Error: Unable to allocate memory.\n");
            exit(1);
        }
        found_strings = (string_clusters_t**)temp_pointer;
        found_strings[stringclusters_found-1] = malloc(sizeof(string_clusters_t));
        if (found_strings[stringclusters_found-1] == NULL){
            printf("Error: Unable to allocate memory.\n");
            exit(1);
        }
        found_strings[stringclusters_found-1]->start_bpos = header_info.last_header_bpos+i+1;
        found_strings[stringclusters_found-1]->length = searchoffset;
        found_strings[stringclusters_found] = NULL;

        offset += searchoffset;
    }
    return stringclusters_found;
}

//Search for new strings. For now, just assume extra space before header if left is leaked string. Returns stringclusters found.
int searchNewStrings(int size_found)
{
    if (new_strings != NULL) {
        // free the memory from the previous found strings
        for (int i = 0; new_strings[i]; i++)
            free(new_strings[i]);

        free(new_strings);
        new_strings = NULL;
    }
    int search_size = header_info.offset - size_found;

    if (search_size <= 0 || !search_for_new_strings) return 0;

    void *temp_pointer;
    int stringclusters_found = 0;

    //todo: actually make a proper search function.
    temp_pointer = realloc(new_strings, (++stringclusters_found+1)*sizeof(string_clusters_t*)); //preincrement
    if (temp_pointer == NULL){
        printf("Error: Unable to allocate memory.\n");
        exit(1);
    }
    new_strings = (string_clusters_t**)temp_pointer;
    new_strings[stringclusters_found-1] = malloc(sizeof(string_clusters_t));
    if (new_strings[stringclusters_found-1] == NULL){
        printf("Error: Unable to allocate memory.\n");
        exit(1);
    }
    new_strings[stringclusters_found-1]->start_bpos = header_info.header_bpos;
    new_strings[stringclusters_found-1]->length = search_size;
    new_strings[stringclusters_found] = NULL;

    return stringclusters_found;
}

// This function will also run an additional search on the current file until the end of the file to see if there are any special leaked strings there
// it's pretty much a copy-paste of the normal in file check.
int endFileCheck()
{

    //offset from blocks as searching
    int offset = 0;
    void *temp_pointer;
    int stringclusters_found = 0;

    int i = 0;
    if (ignore_in_files) i = header_info.number_of_blocks;
    //start checking the beginning of every block
    for (i = i; !feof(input_file); i++){
        //load offset area of header into memory
        //Even better than before! The limit is 512 bytes!
        if (input_bpos != header_info.header_bpos+i+1){
            fseek(input_file, (header_info.header_bpos+i+1)*BLOCK_SIZE + global_offset + offset + header_info.offset, SEEK_SET);
            int read_size = fread(buffer, 1, BLOCK_SIZE, input_file);
            if (read_size != BLOCK_SIZE) {
                if (feof(input_file)) break;
                printf("Reading error\n");
                exit(1);
            }
            input_bpos = header_info.header_bpos+i+1;
        }

        //check for strings
        //search loop like above
        int searchoffset = 0;
        int last_searchoffset = -1;
        while (last_searchoffset != searchoffset){
            last_searchoffset = searchoffset;
            for (int i = 0; leaked_strings[i] != NULL; i++){
                int temp_stringlen = strlen(leaked_strings[i]);

                //check for string
                for (int x = 0; x < temp_stringlen; x++)
                    if (buffer[x+searchoffset] != leaked_strings[i][x]){
                        temp_stringlen = 0; //string not found
                        break;
                    }

                //add to the sizes found
                searchoffset += temp_stringlen;

                if (temp_stringlen != 0) break; //reset loop
            }

            if (last_searchoffset != searchoffset) continue;

            //added loop. having a separate function really would've been more elegant, but again, I didn't want to make any more functions
            for (int i = 0; leaked_strings_end[i] != NULL; i++){
                int temp_stringlen = strlen(leaked_strings_end[i]);

                //check for string
                for (int x = 0; x < temp_stringlen; x++)
                    if (buffer[x+searchoffset] != leaked_strings_end[i][x]){
                        temp_stringlen = 0; //string not found
                        break;
                    }

                //add to the sizes found
                searchoffset += temp_stringlen;

                if (temp_stringlen != 0) break; //reset loop
            }

            if (additional_strings == NULL || last_searchoffset != searchoffset) continue;

            for (int i = 0; additional_strings[i] != NULL; i++){
                uint8_t temp_stringlen = additional_strings[i]->length;

                //check for string in reverse
                for (int x = 0; x < temp_stringlen; x++)
                    if (buffer[x+searchoffset] != additional_strings[i]->string[x]){
                        temp_stringlen = 0; //string not found
                        break;
                    }

                //add to the sizes found
                searchoffset += temp_stringlen;

                if (temp_stringlen != 0) break; //reset loop
            }
        }

        //since eof flag is set, rewind to unset it.
        rewind(input_file);
        input_bpos = 0;

        //continue if nothing found
        if (searchoffset == 0) continue;

        //otherwise add to the list of found strings
        temp_pointer = realloc(end_strings, (++stringclusters_found+1)*sizeof(string_clusters_t*)); //preincrement
        if (temp_pointer == NULL){
            printf("Error: Unable to allocate memory.\n");
            exit(1);
        }
        end_strings = (string_clusters_t**)temp_pointer;
        end_strings[stringclusters_found-1] = malloc(sizeof(string_clusters_t));
        if (end_strings[stringclusters_found-1] == NULL){
            printf("Error: Unable to allocate memory.\n");
            exit(1);
        }
        end_strings[stringclusters_found-1]->start_bpos = header_info.header_bpos+i+1;
        end_strings[stringclusters_found-1]->length = searchoffset;
        end_strings[stringclusters_found] = NULL;

        offset += searchoffset;
    }
    return stringclusters_found;
}

// Copy from input file to output file given starting block position, ending block position (which is also copied), and offset of start
// NOTE: this DOES use the global offset value. This means that if nextHeaderOffset is called, this can't be used anymore.
// If I ever recode this better, I'll actually make it possible to use in a library. For now, I'm getting the function and the algorithms.
// returns # of blocks fully copied.
int copyBlocks(int start_bpos, int end_bpos, int block_offset)
{
    //set the input position
    fseek(input_file, start_bpos*BLOCK_SIZE+global_offset+block_offset, SEEK_SET);
    input_bpos = start_bpos;

    //set the output position
    fseek(output_file, start_bpos*BLOCK_SIZE, SEEK_SET);
    output_bpos = start_bpos;

    int num_blocks_copy = end_bpos - start_bpos + 1;

    //copy the blocks, pairs at a time.
    for (int i = 0; i < num_blocks_copy/COPY_BLOCK_SIZE; i++){
        int read_size = fread(copy_buffer, 1, BLOCK_SIZE*COPY_BLOCK_SIZE, input_file);
        if (ferror(input_file)) {
            printf("Reading error\n");
            exit(1);
        }
        input_bpos += COPY_BLOCK_SIZE;

        fwrite(copy_buffer, sizeof(char), read_size, output_file);
        if (ferror(output_file)) {
            printf("Output file error\n");
            exit(1);
        }
        output_bpos += COPY_BLOCK_SIZE;

        //exit early if unexpected EOF
        if (feof(input_file)){
            return i*COPY_BLOCK_SIZE + read_size/BLOCK_SIZE;
        }

    }

    //if there are additional blocks to copy, copy them.
    if (num_blocks_copy%COPY_BLOCK_SIZE != 0){
        int read_size = fread(copy_buffer, 1, (num_blocks_copy%COPY_BLOCK_SIZE)*BLOCK_SIZE, input_file);
        if (ferror(input_file)) {
            printf("Reading error\n");
            exit(1);
        }
        input_bpos += num_blocks_copy%COPY_BLOCK_SIZE;

        fwrite(copy_buffer, sizeof(char), read_size, output_file);
        if (ferror(output_file)) {
            printf("Output file error\n");
            exit(1);
        }
        output_bpos += num_blocks_copy%COPY_BLOCK_SIZE;

        //exit early if unexpected EOF
        if (feof(input_file)){
            return num_blocks_copy - num_blocks_copy%COPY_BLOCK_SIZE + read_size/BLOCK_SIZE;
        }
    }

    //write changes in output file buffer to file.
    fflush(output_file);

    return num_blocks_copy;
}
