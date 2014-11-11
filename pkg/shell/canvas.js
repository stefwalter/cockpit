/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

var shell = shell || { };
(function($, shell) {

/* Construct a simple image editor inside 'element'.  It can only crop
 * an image to a square region.
 *
 * - editor = shell.image_editor(element, width, height)
 *
 * - editor.load_data(data)
 *
 * - editor.get_data()
 */

shell.image_editor = function image_editor(element, width, height) {
    var self = {
        load_data: load_data
    };

    var square_size = Math.min (width, height);
    var $image_canvas, $overlay_canvas;
    var image_canvas, overlay_canvas;
    var image_2d, overlay_2d;

    var crop_handle_width = 20;

    function setup() {
        element.empty().
            css('width', width).
            css('height', height).
            css('position', 'relative').
            append(
                $image_canvas = $('<canvas>'),
                $overlay_canvas = $('<canvas>').
                    css('position', 'absolute').
                    css('top', 0).
                    css('left', 0).
                    css('z-index', 10));
        image_canvas = $image_canvas[0];
        image_2d = image_canvas.getContext("2d");
        overlay_canvas = $overlay_canvas[0];
        overlay_2d = overlay_canvas.getContext("2d");
        image_canvas.width = overlay_canvas.width = width;
        image_canvas.height = overlay_canvas.height = height;

        set_crop ((width - square_size) / 2, (height - square_size) / 2, square_size, true);
        $overlay_canvas.on('mousedown', mousedown);
    }

    var crop_x, crop_y, crop_s;

    function set_crop(x, y, s, fix) {
        function clamp (low, val, high) {
            if (val < low)
                return low;
            if (val > high)
                return high;
            return val;
        }

        x = Math.floor(x);
        y = Math.floor(y);
        s = Math.floor(s);

        var min_s = 2 * crop_handle_width;

        if (fix) {
            // move it until it fits
            s = clamp (min_s, s, square_size);
            x = clamp (0, x, width - s);
            y = clamp (0, y, height - s);
        } else if (x < 0 || y < 0 || x + s > width || y + s > height || s < min_s)
            return;

        crop_x = x;
        crop_y = y;
        crop_s = s;

        draw_crop (x, y, x+s, y+s);
    }

    function draw_crop(x1,y1,x2,y2) {
        var ctxt = overlay_2d;

        function draw_box (x1, y1, x2, y2) {
            ctxt.strokeStyle = 'black';
            ctxt.strokeRect(x1+0.5, y1+0.5, x2-x1-1, y2-y1-1);
            ctxt.strokeStyle = 'white';
            ctxt.strokeRect(x1+1.5, y1+1.5, x2-x1-3, y2-y1-3);
        }

        ctxt.clearRect(0, 0, width, height);
        ctxt.fillStyle = 'rgba(0,0,0,0.8)';
        ctxt.fillRect(0, 0, width, height);
        ctxt.clearRect(x1, y1, x2 - x1, y2 - y1);

        var h_w = crop_handle_width;
        draw_box (x1, y1, x1+h_w, y1+h_w);
        draw_box (x2-h_w, y1, x2, y1+h_w);
        draw_box (x1, y2-h_w, x1+h_w, y2);
        draw_box (x2-h_w, y2-h_w, x2, y2);
        draw_box (x1, y1, x2, y2);
    }

    function mousedown(ev) {
        var offset = $overlay_canvas.offset();
        var xoff = ev.pageX - offset.left - crop_x;
        var yoff = ev.pageY - offset.top - crop_y;

        var orig_x = crop_x;
        var orig_y = crop_y;
        var orig_s = crop_s;

        var proj_sign, dx_sign, dy_sign, ds_sign;

        var h_w = crop_handle_width;

        if (xoff > 0 && yoff > 0 && xoff < crop_s && yoff < crop_s) {
            if (xoff < h_w && yoff < h_w) {
                // top left
                proj_sign = 1;
                dx_sign = 1;
                dy_sign = 1;
                ds_sign = -1;
            } else if (xoff > crop_s - h_w && yoff < h_w) {
                // top right
                proj_sign = -1;
                dx_sign = 0;
                dy_sign = -1;
                ds_sign = 1;
            } else if (xoff < h_w && yoff > crop_s - h_w) {
                // bottom left
                proj_sign = -1;
                dx_sign = 1;
                dy_sign = 0;
                ds_sign = -1;
            } else if (xoff > crop_s - h_w && yoff > crop_s - h_w) {
                // bottom right
                proj_sign = 1;
                dx_sign = 0;
                dy_sign = 0;
                ds_sign = 1;
            } else {
                // center
                proj_sign = 0;
            }

            function mousemove(ev) {
                var x = ev.pageX - offset.left - xoff;
                var y = ev.pageY - offset.top - yoff;
                if (proj_sign === 0)
                    set_crop (x, y, orig_s, true);
                else {
                    var d = Math.floor((x - orig_x + proj_sign * (y - orig_y)) / 2);
                    set_crop (orig_x + dx_sign*d, orig_y + dy_sign*d, orig_s + ds_sign*d, false);
                }
            }

            function mouseup(ev) {
                $('body').off('mousemove', mousemove);
                $('body').off('mouseup', mouseup);
            }

            $('body').on('mousemove', mousemove);
            $('body').on('mouseup', mouseup);
        }
    }

    function load_data(data) {
        var img = new window.Image();
        img.onerror = function () {
            console.log("failed to load image into canvas");
        };
        img.onload = function () {
            image_2d.clearRect(0, 0, width, height);
            image_2d.drawImage(img, 0, 0, width, height);
        };
        img.src = data;
    }

    setup();

    return self;
};

})(jQuery, shell);
