use super::parse_str;

#[test]
fn parse_comment() {
    parse_str(
        "//
        ////
        ///////////////////////////////////////////////////////// asda
        //////////////////// dad ////////// /
        /////////////////////////////////////////////////////////////////////////////////////////////////////
        //
    ",
    )
    .unwrap();
}

#[test]
fn parse_types() {
    parse_str("const a : i32 = 2;").unwrap();
    assert!(parse_str("const a : x32 = 2;").is_err());
    parse_str("var t: texture_2d<f32>;").unwrap();
    parse_str("var t: texture_cube_array<i32>;").unwrap();
    parse_str("var t: texture_multisampled_2d<u32>;").unwrap();
    parse_str("var t: texture_storage_1d<rgba8uint,write>;").unwrap();
    parse_str("var t: texture_storage_3d<r32float,read>;").unwrap();
}

#[test]
fn parse_type_inference() {
    parse_str(
        "
        fn foo() {
            let a = 2u;
            let b: u32 = a;
            var x = 3.;
            var y = vec2<f32>(1, 2);
        }",
    )
    .unwrap();
    assert!(parse_str(
        "
        fn foo() { let c : i32 = 2.0; }",
    )
    .is_err());
}

#[test]
fn parse_type_cast() {
    parse_str(
        "
        const a : i32 = 2;
        fn main() {
            var x: f32 = f32(a);
            x = f32(i32(a + 1) / 2);
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            let x: vec2<f32> = vec2<f32>(1.0, 2.0);
            let y: vec2<u32> = vec2<u32>(x);
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            let x: vec2<f32> = vec2<f32>(0.0);
        }
    ",
    )
    .unwrap();
    assert!(parse_str(
        "
        fn main() {
            let x: vec2<f32> = vec2<f32>(0);
        }
    ",
    )
    .is_err());
}

#[test]
fn parse_struct() {
    parse_str(
        "
        struct Foo { x: i32 }
        struct Bar {
            @size(16) x: vec2<i32>,
            @align(16) y: f32,
            @size(32) @align(128) z: vec3<f32>,
        };
        struct Empty {}
        var<storage,read_write> s: Foo;
    ",
    )
    .unwrap();
}

#[test]
fn parse_standard_fun() {
    parse_str(
        "
        fn main() {
            var x: i32 = min(max(1, 2), 3);
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_statement() {
    parse_str(
        "
        fn main() {
            ;
            {}
            {;}
        }
    ",
    )
    .unwrap();

    parse_str(
        "
        fn foo() {}
        fn bar() { foo(); }
    ",
    )
    .unwrap();
}

#[test]
fn parse_if() {
    parse_str(
        "
        fn main() {
            if true {
                discard;
            } else {}
            if 0 != 1 {}
            if false {
                return;
            } else if true {
                return;
            } else {}
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_parentheses_if() {
    parse_str(
        "
        fn main() {
            if (true) {
                discard;
            } else {}
            if (0 != 1) {}
            if (false) {
                return;
            } else if (true) {
                return;
            } else {}
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_loop() {
    parse_str(
        "
        fn main() {
            var i: i32 = 0;
            loop {
                if i == 1 { break; }
                continuing { i = 1; }
            }
            loop {
                if i == 0 { continue; }
                break;
            }
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            var found: bool = false;
            var i: i32 = 0;
            while !found {
                if i == 10 {
                    found = true;
                }

                i = i + 1;
            }
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            while true {
                break;
            }
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            var a: i32 = 0;
            for(var i: i32 = 0; i < 4; i = i + 1) {
                a = a + 2;
            }
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        fn main() {
            for(;;) {
                break;
            }
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_switch() {
    parse_str(
        "
        fn main() {
            var pos: f32;
            switch (3) {
                case 0, 1: { pos = 0.0; }
                case 2: { pos = 1.0; }
                default: { pos = 3.0; }
            }
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_switch_optional_colon_in_case() {
    parse_str(
        "
        fn main() {
            var pos: f32;
            switch (3) {
                case 0, 1 { pos = 0.0; }
                case 2 { pos = 1.0; }
                default { pos = 3.0; }
            }
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_switch_default_in_case() {
    parse_str(
        "
        fn main() {
            var pos: f32;
            switch (3) {
                case 0, 1: { pos = 0.0; }
                case 2: {}
                case default, 3: { pos = 3.0; }
            }
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_parentheses_switch() {
    parse_str(
        "
        fn main() {
            var pos: f32;
            switch pos > 1.0 {
                default: { pos = 3.0; }
            }
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_texture_load() {
    parse_str(
        "
        var t: texture_3d<u32>;
        fn foo() {
            let r: vec4<u32> = textureLoad(t, vec3<u32>(0.0, 1.0, 2.0), 1);
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        var t: texture_multisampled_2d_array<i32>;
        fn foo() {
            let r: vec4<i32> = textureLoad(t, vec2<i32>(10, 20), 2, 3);
        }
    ",
    )
    .unwrap();
    parse_str(
        "
        var t: texture_storage_1d_array<r32float,read>;
        fn foo() {
            let r: vec4<f32> = textureLoad(t, 10, 2);
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_texture_store() {
    parse_str(
        "
        var t: texture_storage_2d<rgba8unorm,write>;
        fn foo() {
            textureStore(t, vec2<i32>(10, 20), vec4<f32>(0.0, 1.0, 2.0, 3.0));
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_texture_query() {
    parse_str(
        "
        var t: texture_multisampled_2d_array<f32>;
        fn foo() {
            var dim: vec2<u32> = textureDimensions(t);
            dim = textureDimensions(t, 0);
            let layers: u32 = textureNumLayers(t);
            let samples: u32 = textureNumSamples(t);
        }
    ",
    )
    .unwrap();
}

#[test]
fn parse_postfix() {
    parse_str(
        "fn foo() {
        let x: f32 = vec4<f32>(1.0, 2.0, 3.0, 4.0).xyz.rgbr.aaaa.wz.g;
        let y: f32 = fract(vec2<f32>(0.5, x)).x;
    }",
    )
    .unwrap();
}

#[test]
fn parse_expressions() {
    parse_str("fn foo() {
        let x: f32 = select(0.0, 1.0, true);
        let y: vec2<f32> = select(vec2<f32>(1.0, 1.0), vec2<f32>(x, x), vec2<bool>(x < 0.5, x > 0.5));
        let z: bool = !(0.0 == 1.0);
    }").unwrap();
}

#[test]
fn parse_pointers() {
    parse_str(
        "fn foo() {
        var x: f32 = 1.0;
        let px = &x;
        let py = frexp(0.5, px);
    }",
    )
    .unwrap();
}

#[test]
fn parse_struct_instantiation() {
    parse_str(
        "
    struct Foo {
        a: f32,
        b: vec3<f32>,
    }

    @fragment
    fn fs_main() {
        var foo: Foo = Foo(0.0, vec3<f32>(0.0, 1.0, 42.0));
    }
    ",
    )
    .unwrap();
}

#[test]
fn parse_array_length() {
    parse_str(
        "
        struct Foo {
            data: array<u32>
        } // this is used as both input and output for convenience

        @group(0) @binding(0)
        var<storage> foo: Foo;

        @group(0) @binding(1)
        var<storage> bar: array<u32>;

        fn baz() {
            var x: u32 = arrayLength(foo.data);
            var y: u32 = arrayLength(bar);
        }
        ",
    )
    .unwrap();
}

#[test]
fn parse_storage_buffers() {
    parse_str(
        "
        @group(0) @binding(0)
        var<storage> foo: array<u32>;
        ",
    )
    .unwrap();
    parse_str(
        "
        @group(0) @binding(0)
        var<storage,read> foo: array<u32>;
        ",
    )
    .unwrap();
    parse_str(
        "
        @group(0) @binding(0)
        var<storage,write> foo: array<u32>;
        ",
    )
    .unwrap();
    parse_str(
        "
        @group(0) @binding(0)
        var<storage,read_write> foo: array<u32>;
        ",
    )
    .unwrap();
}
