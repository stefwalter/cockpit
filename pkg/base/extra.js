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

var extra = extra || { };

define([
    'jquery',
    './cockpit'
], function($, cockpit) {

    var extra = { };

    /* ----------------------------------------------------------------------------
     * Bar Graphs (in table rows)
     *
     * <td>
     *    <div class="bar-row" graph="name" value="50/100"/>
     * </td>
     * <td>
     *    <div class="bar-row" graph="name" value="80"/>
     * </td>
     *
     * The various rows must have class="bar-row". Add the "bar-row-danger"
     * class if you want the bar to show up as a warning (ie: red)
     *
     * The graph="xxx" attribute must for all the bars that are part of the
     * same graph, in order for the rows lengths to be coordinated with one
     * another. Length are based on percentage, so the parent of each
     * div.bar-row must be the same size.
     *
     * value="x" can either be a number, or two numbers separated by a slash
     * the latter one is the limit. Change the attribute on the DOM and the
     * graph should update a short while later.
     *
     * On document creation any div.bar-row are automatically turned into
     * Bar graphs. Or use extra.BarRow('name') constructor.
     *
     * You can also use the el.reflow() function on the element to reflow
     * the corresponding graph.
     */

    (function($, extra) {

        function reflow_bar_graph(graph, div) {
            var parts;
            if (graph) {
                var selector = "div.bar-row[graph='" + graph + "']";
                parts = $(selector);
            } else if (div) {
                parts = $(div);
            } else {
                parts = $([]);
            }

            function value_parts(el) {
                var value = $(el).attr('value');
                if (value === undefined)
                    return [NaN];
                var values = value.split("/", 2);
                var portion = parseInt(values[0], 10);
                if (values.length == 1)
                    return [portion];
                var limit = parseInt(values[1], 10);
                if (!isNaN(limit) && portion > limit)
                    portion = limit;
                if (portion < 0)
                    portion = 0;
                return [portion, limit];
            }

            /* One pass to calculate the absolute maximum */
            var max = 0;
            parts.each(function() {
                var limit = value_parts(this).pop();
                if (!isNaN(limit) && limit > max)
                    max = limit;
            });

            /* Max gets rounded up to the nearest 100 MiB for sets of bar rows
             */
            if (graph) {
                var bound = 100*1024*1024;
                max = max - (max % bound) + bound;
            }

            /* Now resize everything to the right aspect */
            parts.each(function() {
                var bits = value_parts(this);
                var portion = bits.shift();
                var limit = bits.pop();
                if (isNaN(portion) || limit === 0) {
                    $(this).css("visibility", "hidden");
                } else {
                    var bar_progress = $(this).data('bar-progress');
                    if (isNaN(limit)) {
                        bar_progress.addClass("progress-no-limit");
                        limit = portion;
                    } else {
                        bar_progress.removeClass("progress-no-limit");
                    }
                    $(this).css('visibility', 'visible');
                    bar_progress.css("width", ((limit / max) * 100) + "%");
                    $(this).data('bar-progress-bar').
                        css('width', ((portion / limit) * 100) + "%").
                        toggle(portion > 0);
                }
            });
        }

        var reflow_timeouts = { };
        function reflow_bar_graph_soon(graph, div) {
            if (graph === undefined) {
                if (div)
                    graph = $(div).attr("graph");
            }

            /* If no other parts to this bar, no sense in waiting */
            if (!graph) {
                reflow_bar_graph(undefined, div);
                return;
            }

            /* Wait until later in case other updates come in to other bits */
            if (reflow_timeouts[graph] !== "undefined")
                window.clearTimeout(reflow_timeouts[graph]);
            reflow_timeouts[graph] = window.setTimeout(function() {
                delete reflow_timeouts[graph];
                reflow_bar_graph(graph);
            }, 10);
        }

        function setup_bar_graph(div) {

            /*
             * We consume <div class="bar-row"> elements and turn them into:
             *
             * <div class="bar-row">
             *    <div class="progress">
             *      <div class="progress-bar">
             *    </div>
             * </div>
             */
            var progress_bar = $("<div>").addClass("progress-bar");
            var progress = $("<div>").addClass("progress").append(progress_bar);
            $(div).
                addClass('bar-row').
                append(progress).
                data('bar-progress', progress).
                data('bar-progress-bar', progress_bar);

            /* see attrchange.js: http://meetselva.github.io/attrchange/ */
            $(div).attrchange({
                trackValues: false,
                callback: function(event) {
                    if (event.attributeName == "graph" ||
                        event.attributeName == "value") {
                        reflow_bar_graph_soon(this.getAttribute("graph"), this);
                    }
                }
            });

            /* Public API */
            div.reflow = function() {
                reflow_bar_graph(this.getAttribute("graph"), this);
            };

            reflow_bar_graph_soon(undefined, div);
        }

        function setup_bar_graphs() {
            $("div.bar-row").each(function() {
                setup_bar_graph(this, false);
            });
        }

        $(document).ready(setup_bar_graphs);

        /* Public API */
        extra.BarRow = function BarRow(graph) {
            var div = $("<div>").addClass('bar-row').attr('graph', graph);
            setup_bar_graph(div);
            return div;
        };

    })($, extra);

    /* ----------------------------------------------------------------------------
     * Sliders
     *
     * <div class="slider" value="0.5">
     *    <div class="slider-bar">
     *        <div class="slider-thumb"></div>
     *    </div>
     *    <div class="slider-bar">
     *        <!-- optional left overs -->
     *    </div>
     * </div>
     *
     * A slider control. The first div.slider-bar is the one that is resized.
     * The value will be bounded between 0 and 1 as a floating point number.
     *
     * The following div.slider-bar if present is resized to fill the remainder
     * of the slider if not given a specific size. You can put more div.slider-bar
     * inside it to reflect squashing other prevous allocations.
     *
     * If the following div.slider-bar have a width specified, then the
     * slider supports the concept of overflowing. If the slider overflows
     * it will get the .slider-warning class and go a bit red.
     *
     * On document creation any div.slider are automatically turned into
     * Bar graphs. Or use extra.Slider() constructor.
     *
     * Slider has the following extra read/write properties:
     *
     * .value: the floating point value the slider is set to.
     * .disabled: whether to display slider as disabled and refuse interacton.
     *
     * Slider has this event:
     *
     * on('change'): fired when the slider changes, passes value as additional arg.
     */

    (function($, extra) {

        function resize_flex(slider, flex, total, part) {
            var value = 0;
            if (part > total)
                value = 1;
            else if (part < 0 || isNaN(part))
                value = 0;
            else if (!isNaN(total) && total > 0 && part >= 0)
                value = (part / total);
            $(flex).css('width', (value * 100) + "%").
                next("div").css('margin-left', $(flex).css('width'));

            /* Set the property and the attribute */
            slider.value = value;
        }

        function update_value(slider) {
            resize_flex(slider, $(slider).children("div.slider-bar").first()[0], 1, slider.value);
        }

        function check_overflow(slider) {
            $(slider).toggleClass("slider-warning",
                                  slider.offsetWidth < slider.scrollWidth);
        }

        function setup_slider(slider) {
            $(slider).attr('unselectable', 'on');

            Object.defineProperty(slider, "value", {
                get: function() {
                    return parseFloat(this.getAttribute("value"));
                },
                set: function(v) {
                    var s = String(v);
                    if (s != this.getAttribute("value"))
                        this.setAttribute("value", v);
                }
            });

            Object.defineProperty(slider, "disabled", {
                get: function() {
                    if (!this.hasAttribute("disabled"))
                        return false;
                    return this.getAttribute("disabled").toLowerCase() != "false";
                },
                set: function(v) {
                    this.setAttribute("disabled", v ? "true" : "false");
                }
            });

            update_value(slider);
            check_overflow(slider);

            /* see attrchange.js: http://meetselva.github.io/attrchange/ */
            $(slider).attrchange({
                trackValues: true,
                callback: function(event) {
                    if (event.attributeName == "value" && event.oldValue !== event.newValue)
                        update_value(slider);
                    if (event.attributeName == "disabled")
                        $(slider).toggleClass("slider-disabled", slider.disabled);
                }
            });

            if (slider.disabled)
                $(slider).addClass("slider-disabled");

            $(slider).on("mousedown", function(ev) {
                if (slider.disabled)
                    return true; /* default action */
                var flex;
                var offset = $(slider).offset().left;
                if ($(ev.target).hasClass("slider-thumb")) {
                    var hitx  = (ev.offsetX || ev.clientX - $(ev.target).offset().left);
                    offset += (hitx - $(ev.target).outerWidth() / 2);
                    flex = $(ev.target).parent()[0];
                } else {
                    flex = $(slider).children("div.slider-bar").first()[0];
                    resize_flex(slider, flex, $(slider).width(), (ev.pageX - offset));
                    $(slider).trigger("change", [slider.value]);
                    check_overflow(slider);
                }

                $(document).
                    on("mousemove.slider", function(ev) {
                        resize_flex(slider, flex, $(slider).width(), (ev.pageX - offset));
                        $(slider).trigger("change", [slider.value]);
                        check_overflow(slider);
                        return false;
                    }).
                    on("mouseup.slider", function(ev) {
                        $(document).
                            off("mousemove.slider").
                            off("mouseup.slider");
                        return false;
                    });
                return false; /* no default action */
            });
        }

        function setup_sliders() {
            $("div.slider").each(function() {
                setup_slider(this);
            });
        }

        $(document).ready(setup_sliders);

        /* Public API */
        extra.Slider = function Slider() {
            var div = $("<div class='slider'>").
                append($("<div class='slider-bar'>").
                    append($("<div class='slider-thumb'>")));
            setup_slider(div);
            return div;
        };

    })($, extra);

    /* ----------------------------------------------------------------------------
     *
     *  On/Off button.
     *
     * XXX - not via HTML markup yet, only via JavaScript.
     * XXX - Use events, not explicit callbacks.
     *
     * When both ON and OFF are given, they are called without arguments
     * after the button has changed to the corresponding state.
     *
     * When only ON is given, it is called with the new state as the only
     * parameter.
     *
     * When ROLE_CHECK is specified, it is called before the button
     * changes state.  If it returns 'false', the change is declined.
     */

    extra.OnOff = function OnOff(val, on, off, role_check) {
        function toggle(event) {
            if (role_check && !role_check())
                return false;

            box.find('.btn').toggleClass('active');
            box.find('.btn').toggleClass('btn-primary');
            box.find('.btn').toggleClass('btn-default');
            if (on_btn.hasClass('active')) {
                if (off)
                    on();
                else
                    on(true);
            } else {
                if (off)
                    off();
                else
                    on(false);
            }
            return false;
        }

        var on_btn, off_btn;
        var box =
            $('<div class="btn-group btn-toggle">').append(
                on_btn = $('<button class="btn">').
                    text("On").
                    addClass(!val? "btn-default" : "btn-primary active").
                    click(toggle),
                off_btn = $('<button class="btn">').
                    text("Off").
                    addClass(val? "btn-default" : "btn-primary active").
                    click(toggle));

        box.set = function set(val) {
            (val? on_btn : off_btn).addClass("btn-primary active").removeClass("btn-default");
            (val? off_btn : on_btn).removeClass("btn-primary active").addClass("btn-default");
        };

        box.enable = function enable(val) {
            box.find('button').prop('disabled', !val);
        };

        return box;
    };

    /* A thin abstraction over flot and metrics channels.  It mostly
     * shields you from hairy array acrobatics and having to know when it
     * is safe or required to create the flot object.
     *
     *
     * - plot = extra.plot(element, x_range)
     *
     * Creates a 'plot' object attached to the given DOM element.  It will
     * show 'x_range' seconds worth of samples.
     *
     * - plot.start_walking(interval)
     *
     * Scroll towards the future every 'interval' seconds.
     *
     * - plot.stop_walking()
     *
     * Stop automatic scrolling.
     *
     * - plot.refresh()
     *
     * Draw the plot.
     *
     * - plot.resize()
     *
     * Resize the plot to fit into its DOM element.  This will
     * automatically refresh the plot.  You should also call this function
     * when 'element' has changed visibility as that might affect its
     * size.
     *
     * - plot.set_options(options)
     *
     * Set the global flot options.  You need to refresh the plot
     * afterwards.
     *
     * In addition to the flot options, you can also set 'setup_hook'
     * field to a function.  This function will be called between
     * flot.setData() and flot.draw() and can be used to adjust the axes
     * limits, for example.  It is called with the flot object as its only
     * parameter.
     *
     * - plot.reset()
     *
     * Resets the plot to be empty.  The plot will disappear completely
     * from the DOM, including the grid.
     *
     * - series = plot.add_metrics_sum_series(desc, options)
     *
     * Adds a single series into the plot that is fed by a metrics
     * channel.  The series will have the given flot options.  The plot
     * will automatically refresh as data becomes available from the
     * channel.
     *
     * The single value for the series is computed by summing the values
     * for all metrics and all instances that are delivered by the
     * channel.
     *
     * The 'desc' argument determines the channel options:
     *
     *   metrics:         An array with the names of all metrics to monitor.
     *   units:           The common units string for all metrics.
     *   instances:       A optional list of instances to include.
     *   omit_instances:  A optional list of instances to omit.
     *   interval:        The interval between samples.
     *   factor:          A factor to apply to the final sum of all samples.
     *
     * - series.options
     *
     * Direct access to the series options.  You need to refresh the plot
     * after changing it.
     *
     * - series.move_to_front()
     *
     * Move the series in front of all other series.  You need to refresh
     * the plot to see the effect immediately.
     *
     * - series.remove()
     *
     * Removes the series from its plot.  The plot will be refreshed.
     *
     * - $(series).on('hover', function (event, val) { ... })
     *
     * This event is triggered when the user hovers over the series ('val'
     * == true), or stops hovering over it ('val' == false).
     */

    extra.plot = function plot(element, x_range) {
        var options = { };
        var data = [ ];
        var flot = null;
        var generation = 0;

        var now = 0;
        var walk_timer;

        function refresh() {
            if (flot === null) {
                if (element.height() === 0 || element.width() === 0)
                    return;
                flot = $.plot(element, data, options);
            }

            flot.setData(data);
            var axes = flot.getAxes();

            axes.xaxis.options.min = now - x_range;
            axes.xaxis.options.max = now;
            if (options.setup_hook)
                options.setup_hook(flot);

            /* This makes sure that the axes are displayed even for an
             * empty plot.
             */
            axes.xaxis.show = true;
            axes.xaxis.used = true;
            axes.yaxis.show = true;
            axes.yaxis.used = true;

            flot.setupGrid();
            flot.draw();
        }

        function start_walking(interval) {
            if (!walk_timer)
                walk_timer = window.setInterval(function () {
                    refresh();
                    now += interval;
                }, interval*1000);
        }

        function stop_walking() {
            if (walk_timer)
                window.clearInterval(walk_timer);
            walk_timer = null;
        }

        function reset() {
            stop_walking();
            for (var i = 0; i < data.length; i++)
                data[i].stop();

            options = { };
            data = [ ];
            flot = null;
            $(element).empty();
            generation += 1;
        }

        function resize() {
            if (element.height() === 0 || element.width() === 0)
                return;
            if (flot)
                flot.resize();
            refresh();
        }

        function set_options(opts) {
            options = opts;
            flot = null;
        }

        function add_metrics_sum_series(desc, opts) {
            var series = opts;
            var series_data = null;
            var timestamp;
            var channel;
            var cur_samples;

            var self = {
                options: series,
                move_to_front: move_to_front,
                remove: remove
            };

            series.stop = stop;
            series.hover = hover;

            function stop() {
                channel.close();
            }

            function hover(val) {
                $(self).triggerHandler('hover', [ val ]);
            }

            function add_series() {
                series.data = series_data;
                data.push(series);
            }

            function remove_series() {
                var pos = data.indexOf(series);
                if (pos >= 0)
                    data.splice(pos, 1);
            }

            function move_to_front() {
                var pos = data.indexOf(series);
                if (pos >= 0) {
                    data.splice(pos, 1);
                    data.push(series);
                }
            }

            function trim_series() {
                for (var i = 0; i < series_data.length; i++) {
                    if (series_data[i][0] >= now - x_range) {
                        series_data.splice(0, i);
                        return;
                    }
                }
            }

            var metrics;
            var instances;

            function init() {
                series_data = [];
                trim_series();
                add_series();
                refresh();
                timestamp = now;
            }

            function on_new_sample(samples) {
                var i, j, sum = 0;

                function count_sample(index, cur, samples) {
                    if (samples[index] || samples[index] === 0)
                        cur[index] = samples[index];
                    sum += cur[index];
                }

                for (i = 0; i < metrics.length; i++) {
                    if (instances[i] !== undefined) {
                        for (j = 0; j < instances[i].length; j++)
                            count_sample(j, cur_samples[i], samples[i]);
                    } else
                        count_sample(i, cur_samples, samples);
                }

                trim_series();
                series_data[series_data.length] = [ timestamp, sum*(desc.factor || 1) ];
                timestamp += (desc.interval || 1000) / 1000;
            }

            function remove() {
                stop();
                remove_series();
                refresh();
            }

            metrics = desc.metrics.map(function (n) { return { name: n, units: desc.units }; });

            channel = cockpit.channel({ payload: "metrics1",
                                        source: desc.source || "direct",
                                        metrics: metrics,
                                        instances: desc.instances,
                                        omit_instances: desc.omit_instances,
                                        interval: desc.interval || 1000,
                                        host: desc.host
                                      });
            $(channel).on("close", function (event, message) {
                console.log(message);
            });
            $(channel).on("message", function (event, message) {
                var msg = JSON.parse(message);
                var i;
                if (msg.length) {
                    for (i = 0; i < msg.length; i++) {
                        on_new_sample(msg[i]);
                    }
                } else {
                    instances = msg.metrics.map(function (m) { return m.instances; });
                    cur_samples = [];
                    for (i = 0; i < metrics.length; i++) {
                        if (instances[i] !== null)
                            cur_samples[i] = [];
                    }
                    if (series_data === null)
                        init();
                }
            });

            return self;
        }

        var cur_hover = null;

        function hover(series) {
            if (series !== cur_hover) {
                if (cur_hover && cur_hover.hover)
                    cur_hover.hover(false);
                cur_hover = series;
                if (cur_hover && cur_hover.hover)
                    cur_hover.hover(true);
            }
        }

        function hover_on(event, pos, item) {
            hover(item && item.series);
        }

        function hover_off(event) {
            hover(null);
        }

        $(element).on("plothover", hover_on);
        $(element).on("mouseleave", hover_off);

        set_options({});

        return {
            start_walking: start_walking,
            stop_walking: stop_walking,
            refresh: refresh,
            reset: reset,
            resize: resize,
            set_options: set_options,
            add_metrics_sum_series: add_metrics_sum_series
        };
    };

    return extra;
});
