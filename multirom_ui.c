#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#include "multirom_ui.h"
#include "framebuffer.h"
#include "input.h"
#include "log.h"
#include "listview.h"
#include "util.h"
#include "button.h"
#include "checkbox.h"

#define HEADER_HEIGHT 75
#define TAB_BTN_WIDTH 165

static fb_text *tab_texts[TAB_COUNT] = { 0 };
static fb_rect *selected_tab_rect = NULL;
static int selected_tab = -1;
static void *tab_data = NULL;
static struct multirom_status *mrom_status = NULL;
static struct multirom_rom *selected_rom = NULL;

static pthread_mutex_t rom_mutex = PTHREAD_MUTEX_INITIALIZER;

struct multirom_rom *multirom_ui(struct multirom_status *s)
{
    if(multirom_init_fb() < 0)
        return NULL;

    mrom_status = s;

    selected_tab = -1;
    multirom_ui_init_header();
    multirom_ui_switch(TAB_INTERNAL);

    add_touch_handler(&multirom_ui_touch_handler, NULL);
    start_input_thread();

    int run = 1;
    while(run)
    {
        usleep(100000);

        pthread_mutex_lock(&rom_mutex);
        if(selected_rom)
            run = 0;
        pthread_mutex_unlock(&rom_mutex);

        if(get_last_key() == KEY_POWER)
            run = 0;
    }

    stop_input_thread();
    rm_touch_handler(&multirom_ui_touch_handler, NULL);

    fb_create_msgbox(500, 250);
    if(!selected_rom)
        fb_msgbox_add_text(-1, -1, SIZE_BIG, "Rebooting...");
    else
    {
        fb_msgbox_add_text(-1, 40, SIZE_BIG, "Booting ROM...");
        fb_msgbox_add_text(-1, -1, SIZE_NORMAL, selected_rom->name);
    }

    fb_draw();
    fb_freeze(1);

    multirom_ui_destroy_tab(selected_tab);
    fb_clear();
    fb_close();
    return selected_rom;
}

void multirom_ui_init_header(void)
{
    fb_add_text(15, 5, WHITE, SIZE_EXTRA, "MultiROM");

    static const char *str[] = { "Internal", "USB", "Misc" };

    int x = fb_width - (TAB_BTN_WIDTH*TAB_COUNT);

    int i, text_x, text_y;
    for(i = 0; i < TAB_COUNT; ++i)
    {
        text_x = center_x(x, TAB_BTN_WIDTH, SIZE_NORMAL, str[i]);
        text_y = center_y(0, HEADER_HEIGHT, SIZE_NORMAL);
        tab_texts[i] = fb_add_text(text_x, text_y, WHITE, SIZE_NORMAL, str[i]);

        fb_add_rect(x, 0, 2, HEADER_HEIGHT, WHITE);

        x += TAB_BTN_WIDTH;
    }

    fb_add_rect(0, HEADER_HEIGHT, fb_width, 2, WHITE);
}

void multirom_ui_header_select(int tab)
{
    int i;
    for(i = 0; i < TAB_COUNT; ++i)
        tab_texts[i]->color = (i == tab) ? BLACK : WHITE;

    if(!selected_tab_rect)
        selected_tab_rect = fb_add_rect(0, 0, TAB_BTN_WIDTH, HEADER_HEIGHT, WHITE);

    selected_tab_rect->head.x = fb_width - (TAB_BTN_WIDTH * (TAB_COUNT - tab));
}

void multirom_ui_destroy_tab(int tab)
{
    switch(tab)
    {
        case TAB_INTERNAL:
        case TAB_USB:
            multirom_ui_tab_rom_destroy(tab_data);
            tab_data = NULL;
            break;
        case TAB_MISC:
            break;
    }
}

void multirom_ui_switch(int tab)
{
    if(tab == selected_tab)
        return;

    fb_freeze(1);

    multirom_ui_header_select(tab);

    // destroy old tab
    multirom_ui_destroy_tab(selected_tab);

    // init new tab
    switch(tab)
    {
        case TAB_INTERNAL:
        case TAB_USB:
            tab_data = multirom_ui_tab_rom_init(tab);
            break;
        case TAB_MISC:
            break;
    }

    selected_tab = tab;

    fb_freeze(0);
    fb_draw();
}

void multirom_ui_fill_rom_list(listview *view, int mask)
{
    int i;
    struct multirom_rom *rom;
    void *data;
    listview_item *it;
    for(i = 0; mrom_status->roms && mrom_status->roms[i]; ++i)
    {
        rom = mrom_status->roms[i];

        if(!(M(rom->type) & mask))
            continue;

        data = rom_item_create(rom->name);
        it = listview_add_item(view, rom->id, data);

        if(rom == mrom_status->current_rom)
            listview_select_item(view, it);
    }
}

int multirom_ui_touch_handler(touch_event *ev, void *data)
{
    if(!(ev->changed & TCHNG_REMOVED))
        return -1;

    int x = fb_width - (TAB_BTN_WIDTH*TAB_COUNT);
    if(ev->y > HEADER_HEIGHT || ev->x < x)
        return -1;

    int i, nextx;
    for(i = 0; i < TAB_COUNT; ++i)
    {
        nextx = x + TAB_BTN_WIDTH;
        if(ev->x > x && ev->x < nextx)
        {
            multirom_ui_switch(i);
            return 0;
        }
        x = nextx;
    }

    return -1;
}

#define ROMS_FOOTER_H 130
#define ROMS_HEADER_H 90

#define BOOTBTN_W 300
#define BOOTBTN_H 80

typedef struct 
{
    listview *list;
    button **buttons;
    void **ui_elements;
    fb_text *rom_name;
} tab_roms;

void *multirom_ui_tab_rom_init(int tab_type)
{
    tab_roms *t = malloc(sizeof(tab_roms));
    memset(t, 0, sizeof(tab_roms));

    int base_y = fb_height-ROMS_FOOTER_H;

    // must be before list
    tab_data = (void*)t;
    t->rom_name = fb_add_text(0, center_y(base_y, ROMS_FOOTER_H, SIZE_NORMAL),
                              WHITE, SIZE_NORMAL, "");


    // rom list
    t->list = malloc(sizeof(listview));
    memset(t->list, 0, sizeof(listview));
    t->list->y = HEADER_HEIGHT+ROMS_HEADER_H;
    t->list->w = fb_width;
    t->list->h = fb_height - t->list->y - ROMS_FOOTER_H-20;

    t->list->item_draw = &rom_item_draw;
    t->list->item_hide = &rom_item_hide;
    t->list->item_height = &rom_item_height;
    t->list->item_destroy = &rom_item_destroy;
    t->list->item_selected = &multirom_ui_tab_rom_selected;

    listview_init_ui(t->list);

    static int rom_mask[2] = {
        // TAB_INTERNAL
        (M(ROM_DEFAULT) | M(ROM_ANDROID_INTERNAL) | M(ROM_UBUNTU_INTERNAL)),
        // TAB_USB
        (M(ROM_ANDROID_USB) | M(ROM_UBUNTU_USB)),
    };
    assert(tab_type < 2);

    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    multirom_ui_fill_rom_list(t->list, rom_mask[tab_type]);
    listview_update_ui(t->list);

    int has_roms = (int)(t->list->items == NULL);

    // header
    const char *str[] = { "Select ROM to boot:", "No ROMs in this location!" };
    int x = center_x(0, fb_width, SIZE_BIG, str[has_roms]);
    int y = center_y(HEADER_HEIGHT, ROMS_HEADER_H, SIZE_BIG);
    fb_text *text = fb_add_text(x, y, LBLUE, SIZE_BIG, str[has_roms]);
    list_add(text, &t->ui_elements);

    // footer
    fb_rect *sep = fb_add_rect(0, fb_height-ROMS_FOOTER_H, fb_width, 2, LBLUE);
    list_add(sep, &t->ui_elements);

    button *b = malloc(sizeof(button));
    memset(b, 0, sizeof(button));
    b->x = fb_width - BOOTBTN_W - 20;
    b->y = base_y + (ROMS_FOOTER_H-BOOTBTN_H)/2;
    b->w = BOOTBTN_W;
    b->h = BOOTBTN_H;
    b->clicked = &multirom_ui_tab_rom_boot_btn;
    button_init_ui(b, "Boot", SIZE_BIG);
    button_enable(b, !has_roms);
    list_add(b, &t->buttons);

    // TODO: refresh btn

    return t;
}

void multirom_ui_tab_rom_destroy(void *data)
{
    tab_roms *t = (tab_roms*)data;

    list_clear(&t->buttons, &button_destroy);
    list_clear(&t->ui_elements, &fb_remove_item);

    listview_destroy(t->list);

    fb_rm_text(t->rom_name);

    free(t);
}

void multirom_ui_tab_rom_selected(listview_item *prev, listview_item *now)
{
    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, now->id);
    if(!rom || !tab_data)
        return;

    tab_roms *t = (tab_roms*)tab_data;

    free(t->rom_name->text);
    t->rom_name->text = malloc(strlen(rom->name)+1);
    strcpy(t->rom_name->text, rom->name);

    t->rom_name->head.x = center_x(0, fb_width-BOOTBTN_W-20, SIZE_NORMAL, rom->name);

    fb_draw();
}

void multirom_ui_tab_rom_boot_btn()
{
    if(!tab_data)
        return;

    tab_roms *t = (tab_roms*)tab_data;
    if(!t->list->selected)
        return;

    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, t->list->selected->id);
    if(!rom)
        return;

    pthread_mutex_lock(&rom_mutex);
    selected_rom = rom;
    pthread_mutex_unlock(&rom_mutex);
}
